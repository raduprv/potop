// damage_view: a damage scene in a window, drawn by the soft renderer
// (the very pixels damage_shot saves), with a free camera and the time
// under control: pause, a frame at a time (back too), slow motion.
//
//   damage_view [--scene NAME] [--size WxH] [--frame N]
//               [--camera ex,ey,ez,tx,ty,tz] [--events FILE.txt]
//
//   W A S D          move (Q down, E up, Shift faster)
//   mouse            look round, as in a first person game (up and down
//                    the other way round, the old school way; I turns that)
//   Tab              let the mouse go, take it back (a click takes it back too)
//   left mouse       throw a steel ball where the cross points (Shift: twice as fast)
//   X                a blast where the cross points (Shift: a big one)
//   Backspace        take the last ball or blast back
//   Delete           take them all back
//   Space            pause, go on
//   Right, Left      a frame forward, back (with Shift ten)
//   [ ]              time slower, faster (1/16 to 4 times)
//   R                back to frame 0
//   O                outlines on, off
//   B                debug marks (contacts and their impulses) on, off
//   1 2 ...          another scene, from its frame 0
//   P                save the picture (and the balls and blasts, to a .txt)
//                    and print damage_shot's command for the same picture
//   C                print the camera
//   H                the keys on the screen, on and off
//   Esc              quit

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "command_line.h"
#include "damage_events.h"
#include "damage_scene.h"
#include "soft_render.h"
#include "soft_render_png.h"

#define MOVE_METERS_PER_SECOND 4.0f
#define MOVE_FAST_FACTOR 4.0f
#define LOOK_RADIANS_PER_PIXEL 0.003f
#define PITCH_LIMIT_RADIANS 1.55f
#define TIME_SCALE_SLOWEST 0.0625f
#define TIME_SCALE_FASTEST 4.0f
#define HUD_COLOR 0xFFFFFF
// The window is not drawn more often than this, whatever vsync does
#define WINDOW_FRAMES_PER_SECOND 60
// What the keys throw and set off
#define BALL_START_METERS 0.6f
#define BALL_METERS_PER_SECOND 16.0f
#define BLAST_STRENGTH 600.0f
#define BLAST_BIG_STRENGTH 1800.0f
#define BLAST_REACH_METERS 200.0f
// A blast is put this far before what the cross points at, not inside it
#define BLAST_BACK_METERS 0.15f

typedef struct
{
	float position[3];
	float yaw_radians;                 // 0 looks along -z, turning right as it grows
	float pitch_radians;               // up is positive
} view_camera_t;

static void view_camera_forward(const view_camera_t *view_camera, float forward[3])
{
	forward[0] = sinf(view_camera->yaw_radians) * cosf(view_camera->pitch_radians);
	forward[1] = sinf(view_camera->pitch_radians);
	forward[2] = -cosf(view_camera->yaw_radians) * cosf(view_camera->pitch_radians);
}

static void view_camera_from_render_camera(view_camera_t *view_camera, const soft_render_camera_t *camera)
{
	float forward[3];
	float length;

	forward[0] = camera->target[0] - camera->eye[0];
	forward[1] = camera->target[1] - camera->eye[1];
	forward[2] = camera->target[2] - camera->eye[2];
	length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
	memcpy(view_camera->position, camera->eye, sizeof(view_camera->position));
	view_camera->yaw_radians = atan2f(forward[0], -forward[2]);
	view_camera->pitch_radians = length > 0.0f ? asinf(forward[1] / length) : 0.0f;
}

static void view_camera_to_render_camera(const view_camera_t *view_camera, soft_render_camera_t *camera)
{
	float forward[3];

	view_camera_forward(view_camera, forward);
	memcpy(camera->eye, view_camera->position, sizeof(camera->eye));
	camera->target[0] = view_camera->position[0] + forward[0];
	camera->target[1] = view_camera->position[1] + forward[1];
	camera->target[2] = view_camera->position[2] + forward[2];
}

static void view_camera_move(view_camera_t *view_camera, const bool *keys, float seconds)
{
	float forward[3];
	float right[3];
	float distance = MOVE_METERS_PER_SECOND * seconds;
	float along_forward = 0.0f;
	float along_right = 0.0f;
	float along_up = 0.0f;
	int i;

	if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
	{
		distance *= MOVE_FAST_FACTOR;
	}
	if (keys[SDL_SCANCODE_W])
	{
		along_forward += distance;
	}
	if (keys[SDL_SCANCODE_S])
	{
		along_forward -= distance;
	}
	if (keys[SDL_SCANCODE_D])
	{
		along_right += distance;
	}
	if (keys[SDL_SCANCODE_A])
	{
		along_right -= distance;
	}
	if (keys[SDL_SCANCODE_E])
	{
		along_up += distance;
	}
	if (keys[SDL_SCANCODE_Q])
	{
		along_up -= distance;
	}
	view_camera_forward(view_camera, forward);
	right[0] = cosf(view_camera->yaw_radians);
	right[1] = 0.0f;
	right[2] = sinf(view_camera->yaw_radians);
	for (i = 0; i < 3; i++)
	{
		view_camera->position[i] += forward[i] * along_forward + right[i] * along_right;
	}
	view_camera->position[1] += along_up;
}

static void print_camera(const soft_render_camera_t *camera)
{
	char text[128];

	command_line_write_camera(camera, text, (int)sizeof(text));
	printf("--camera %s\n", text);
	fflush(stdout);
}

// A steel ball from just before the eye, the way it looks
static void throw_ball(const soft_render_camera_t *camera, int frame_index, bool fast)
{
	damage_event_t event;
	float speed = fast ? BALL_METERS_PER_SECOND * 2.0f : BALL_METERS_PER_SECOND;
	int i;

	memset(&event, 0, sizeof(event));
	event.frame_index = frame_index;
	event.kind = DAMAGE_EVENT_BALL;
	for (i = 0; i < 3; i++)
	{
		// (The render camera's target is a meter before its eye)
		float forward = camera->target[i] - camera->eye[i];

		event.position[i] = camera->eye[i] + forward * BALL_START_METERS;
		event.velocity[i] = forward * speed;
	}
	damage_events_add(&event);
}

// A blast at what the middle of the screen points at, if anything is there
static void set_off_blast(const damage_scene_t *scene, const soft_render_camera_t *camera, int frame_index, bool big)
{
	damage_event_t event;
	float forward[3];
	float hit_point[3];
	int i;

	for (i = 0; i < 3; i++)
	{
		forward[i] = camera->target[i] - camera->eye[i];
	}
	if (scene->cast_ray == NULL || !scene->cast_ray(camera->eye, forward, BLAST_REACH_METERS, hit_point))
	{
		return;
	}
	memset(&event, 0, sizeof(event));
	event.frame_index = frame_index;
	event.kind = DAMAGE_EVENT_BLAST;
	event.strength = big ? BLAST_BIG_STRENGTH : BLAST_STRENGTH;
	for (i = 0; i < 3; i++)
	{
		event.position[i] = hit_point[i] - forward[i] * BLAST_BACK_METERS;
	}
	damage_events_add(&event);
}

// Saves what is on the screen (without the keys' help) and says how
// damage_shot draws the same picture
static void save_picture(const damage_scene_t *scene, soft_render_t *renderer, const soft_render_camera_t *camera, int frame_index, bool draw_outlines, bool draw_debug)
{
	char path[96];
	char events_path[96];
	char events_argument[128];
	char camera_text[128];

	snprintf(path, sizeof(path), "damage_%s_%05d.png", scene->name, frame_index);
	if (!soft_render_save_png(renderer, path))
	{
		printf("cannot write '%s'\n", path);
		return;
	}
	// The balls and blasts go with the picture: without them damage_shot
	// would draw the scene as nobody touched it
	events_argument[0] = '\0';
	if (damage_events_count() > 0)
	{
		snprintf(events_path, sizeof(events_path), "damage_%s_%05d.txt", scene->name, frame_index);
		if (!damage_events_save(events_path, scene->name))
		{
			printf("cannot write '%s'\n", events_path);
			return;
		}
		snprintf(events_argument, sizeof(events_argument), " --events %s", events_path);
	}
	command_line_write_camera(camera, camera_text, (int)sizeof(camera_text));
	printf("saved %s (checksum %08X); the same from damage_shot:\n./damage_shot --out shot.png --scene %s --frame %d --size %dx%d --camera %s%s%s%s\n",
		path, (unsigned int)scene->checksum(), scene->name, frame_index, renderer->width, renderer->height, camera_text,
		events_argument, draw_outlines ? "" : " --no-outlines", draw_debug ? " --debug" : "");
	fflush(stdout);
}

static void draw_hud(const damage_scene_t *scene, soft_render_t *renderer, int frame_index, float time_scale, bool paused, float step_milliseconds, float draw_milliseconds, bool show_keys)
{
	char text[200];
	char about_frame[200];

	snprintf(text, sizeof(text), "%s  FRAME %d  T %.2f S  SPEED X%.4g%s\nSTEP %.2f MS  DRAW %.1f MS  %d TRIANGLES",
		scene->name, frame_index, (double)frame_index / (double)DAMAGE_SCENE_FRAMES_PER_SECOND, (double)time_scale, paused ? "  PAUSED" : "",
		(double)step_milliseconds, (double)draw_milliseconds, renderer->triangles_drawn);
	soft_render_text(renderer, 6, 6, 2, HUD_COLOR, text);
	scene->describe(about_frame, (int)sizeof(about_frame));
	soft_render_text(renderer, 6, 44, 1, HUD_COLOR, about_frame);
	if (damage_events_count() > 0)
	{
		snprintf(text, sizeof(text), "%d BALLS AND BLASTS OF YOURS", damage_events_count());
		soft_render_text(renderer, 6, 56, 1, HUD_COLOR, text);
	}
	// The cross the balls and blasts go by
	soft_render_text(renderer, renderer->width / 2 - 5, renderer->height / 2 - 7, 2, HUD_COLOR, "+");
	if (show_keys)
	{
		soft_render_text(renderer, 6, renderer->height - 9 * 18 - 6, 2, HUD_COLOR,
			"W A S D MOVE, Q E DOWN UP, SHIFT FASTER\n"
			"MOUSE: LOOK ROUND (I: UP AND DOWN THE OTHER WAY)   TAB: LET THE MOUSE GO\n"
			"LEFT MOUSE: THROW A BALL   X: A BLAST (SHIFT: STRONGER)\n"
			"BACKSPACE: TAKE THE LAST ONE BACK   DELETE: ALL\n"
			"SPACE: PAUSE   RIGHT LEFT: A FRAME ON, BACK (SHIFT: TEN)\n"
			"[ ]: SLOWER, FASTER   R: FRAME 0   1 2 3: SCENE\n"
			"O: OUTLINES   B: DEBUG MARKS\n"
			"P: SAVE PNG   C: PRINT CAMERA\n"
			"H: THESE KEYS   ESC: QUIT");
	}
}

int main(int argument_count, char **arguments)
{
	int width = 1280;
	int height = 720;
	int frame_index = 0;
	float frame_fraction = 0.0f;
	float time_scale = 1.0f;
	float draw_milliseconds = 0.0f;
	float step_milliseconds = 0.0f;
	const damage_scene_t *scene = damage_scene_default();
	bool camera_given = false;
	bool paused = false;
	bool draw_outlines = true;
	bool draw_debug = false;
	bool show_keys = true;
	bool looking = true;
	bool invert_look = true;
	bool running = true;
	soft_render_camera_t camera;
	soft_render_camera_t given_camera;
	view_camera_t view_camera;
	soft_render_t renderer;
	SDL_Window *window = NULL;
	SDL_Renderer *window_renderer = NULL;
	SDL_Texture *texture = NULL;
	Uint64 last_ticks;
	int i;

	memset(&given_camera, 0, sizeof(given_camera));
	for (i = 1; i + 1 < argument_count; i += 2)
	{
		const char *argument = arguments[i];
		const char *value = arguments[i + 1];

		if (strcmp(argument, "--size") == 0 && command_line_read_size(value, &width, &height))
		{
		}
		else if (strcmp(argument, "--scene") == 0 && damage_scene_find(value) != NULL)
		{
			scene = damage_scene_find(value);
		}
		else if (strcmp(argument, "--frame") == 0 && sscanf(value, "%d", &frame_index) == 1 && frame_index >= 0)
		{
		}
		else if (strcmp(argument, "--camera") == 0 && command_line_read_camera(value, &given_camera))
		{
			camera_given = true;
		}
		else if (strcmp(argument, "--events") == 0 && damage_events_load(value))
		{
		}
		else
		{
			fprintf(stderr, "damage_view: bad argument '%s %s'\nusage: damage_view [--scene NAME] [--size WxH] [--frame N] [--camera ex,ey,ez,tx,ty,tz] [--events FILE.txt]\nscenes:", argument, value);
			for (i = 0; i < damage_scene_count(); i++)
			{
				fprintf(stderr, " %s", damage_scene_get(i)->name);
			}
			fprintf(stderr, "\n");
			return 2;
		}
	}
	scene->default_camera(&camera);
	if (camera_given)
	{
		memcpy(camera.eye, given_camera.eye, sizeof(camera.eye));
		memcpy(camera.target, given_camera.target, sizeof(camera.target));
	}
	view_camera_from_render_camera(&view_camera, &camera);
	if (!scene->open())
	{
		fprintf(stderr, "damage_view: the scene '%s' cannot be made\n", scene->name);
		return 1;
	}

	if (!soft_render_create(&renderer, width, height))
	{
		fprintf(stderr, "damage_view: out of memory\n");
		return 1;
	}
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "damage_view: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	if (!SDL_CreateWindowAndRenderer("Potop damage", width, height, SDL_WINDOW_RESIZABLE, &window, &window_renderer))
	{
		fprintf(stderr, "damage_view: no window: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetRenderVSync(window_renderer, 1);
	// The picture keeps its shape and its pixels whatever the window's size
	SDL_SetRenderLogicalPresentation(window_renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	texture = SDL_CreateTexture(window_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
	if (texture == NULL)
	{
		fprintf(stderr, "damage_view: no texture: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
	// The mouse looks round from the start (Tab lets it go)
	SDL_SetWindowRelativeMouseMode(window, looking);

	last_ticks = SDL_GetTicksNS();
	while (running)
	{
		SDL_Event event;
		Uint64 now_ticks = SDL_GetTicksNS();
		float seconds = (float)(now_ticks - last_ticks) * 1e-9f;
		Uint64 draw_start_ticks;
		Uint64 step_start_ticks;
		int frame_before_stepping;
		bool save_requested = false;

		last_ticks = now_ticks;
		// (A long stop, the window dragged or the program held in a
		// debugger, is not made up for)
		if (seconds > 0.1f)
		{
			seconds = 0.1f;
		}

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT)
			{
				running = false;
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
			{
				if (!looking)
				{
					// The mouse was let go: the click only takes it back
					looking = true;
					SDL_SetWindowRelativeMouseMode(window, true);
				}
				else
				{
					view_camera_to_render_camera(&view_camera, &camera);
					throw_ball(&camera, frame_index, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
				}
			}
			else if (event.type == SDL_EVENT_MOUSE_MOTION && looking)
			{
				// (Inverted: pulling the mouse back looks up, as a stick does)
				float pitch_direction = invert_look ? 1.0f : -1.0f;

				view_camera.yaw_radians += event.motion.xrel * LOOK_RADIANS_PER_PIXEL;
				view_camera.pitch_radians += pitch_direction * event.motion.yrel * LOOK_RADIANS_PER_PIXEL;
				if (view_camera.pitch_radians > PITCH_LIMIT_RADIANS)
				{
					view_camera.pitch_radians = PITCH_LIMIT_RADIANS;
				}
				if (view_camera.pitch_radians < -PITCH_LIMIT_RADIANS)
				{
					view_camera.pitch_radians = -PITCH_LIMIT_RADIANS;
				}
			}
			else if (event.type == SDL_EVENT_KEY_DOWN)
			{
				int frame_jump = (event.key.mod & SDL_KMOD_SHIFT) ? 10 : 1;

				switch (event.key.key)
				{
					case SDLK_ESCAPE:
						running = false;
						break;
					case SDLK_SPACE:
						paused = !paused;
						break;
					case SDLK_RIGHT:
						paused = true;
						frame_index += frame_jump;
						break;
					case SDLK_LEFT:
						paused = true;
						frame_index = frame_index > frame_jump ? frame_index - frame_jump : 0;
						break;
					case SDLK_LEFTBRACKET:
						if (time_scale > TIME_SCALE_SLOWEST)
						{
							time_scale *= 0.5f;
						}
						break;
					case SDLK_RIGHTBRACKET:
						if (time_scale < TIME_SCALE_FASTEST)
						{
							time_scale *= 2.0f;
						}
						break;
					case SDLK_R:
						frame_index = 0;
						frame_fraction = 0.0f;
						break;
					case SDLK_O:
						draw_outlines = !draw_outlines;
						break;
					case SDLK_B:
						draw_debug = !draw_debug;
						break;
					case SDLK_TAB:
						looking = !looking;
						SDL_SetWindowRelativeMouseMode(window, looking);
						break;
					case SDLK_I:
						invert_look = !invert_look;
						break;
					case SDLK_X:
						view_camera_to_render_camera(&view_camera, &camera);
						set_off_blast(scene, &camera, frame_index, (event.key.mod & SDL_KMOD_SHIFT) != 0);
						break;
					case SDLK_BACKSPACE:
						// The scene is made again; the stepping below brings
						// it back to this frame without what was taken away
						if (damage_events_remove_last())
						{
							scene->close();
							scene->open();
						}
						break;
					case SDLK_DELETE:
						if (damage_events_count() > 0)
						{
							damage_events_clear();
							scene->close();
							scene->open();
						}
						break;
					case SDLK_H:
						show_keys = !show_keys;
						break;
					case SDLK_P:
						save_requested = true;
						break;
					case SDLK_C:
						view_camera_to_render_camera(&view_camera, &camera);
						print_camera(&camera);
						break;
					default:
						// A digit: that scene, from its start and its own camera
						if (event.key.key >= SDLK_1 && event.key.key <= SDLK_9 && damage_scene_get((int)(event.key.key - SDLK_1)) != NULL)
						{
							scene->close();
							damage_events_clear();
							scene = damage_scene_get((int)(event.key.key - SDLK_1));
							scene->open();
							scene->default_camera(&camera);
							view_camera_from_render_camera(&view_camera, &camera);
							frame_index = 0;
							frame_fraction = 0.0f;
						}
						break;
				}
			}
		}

		view_camera_move(&view_camera, SDL_GetKeyboardState(NULL), seconds);
		view_camera_to_render_camera(&view_camera, &camera);

		// The scene goes on in whole frames of its own, however long ours take
		frame_before_stepping = frame_index;
		if (!paused)
		{
			frame_fraction += seconds * (float)DAMAGE_SCENE_FRAMES_PER_SECOND * time_scale;
			while (frame_fraction >= 1.0f)
			{
				frame_fraction -= 1.0f;
				frame_index++;
			}
		}
		step_start_ticks = SDL_GetTicksNS();
		scene->seek(frame_index);
		if (frame_index > frame_before_stepping)
		{
			// (Going back steps the scene again from its start: not timed)
			step_milliseconds = (float)(SDL_GetTicksNS() - step_start_ticks) * 1e-6f / (float)(frame_index - frame_before_stepping);
		}

		draw_start_ticks = SDL_GetTicksNS();
		scene->draw(&renderer, &camera, draw_outlines, draw_debug);
		if (save_requested)
		{
			save_picture(scene, &renderer, &camera, frame_index, draw_outlines, draw_debug);
		}
		draw_hud(scene, &renderer, frame_index, time_scale, paused, step_milliseconds, draw_milliseconds, show_keys);
		draw_milliseconds = (float)(SDL_GetTicksNS() - draw_start_ticks) * 1e-6f;

		SDL_UpdateTexture(texture, NULL, renderer.color, width * 4);
		SDL_RenderClear(window_renderer);
		SDL_RenderTexture(window_renderer, texture, NULL, NULL);
		SDL_RenderPresent(window_renderer);

		// Where vsync does not hold the loop back (it does not everywhere)
		// it would spin as fast as it can, a whole core for nothing: the
		// rest of the frame's time is slept away
		{
			Uint64 frame_nanoseconds = 1000000000u / WINDOW_FRAMES_PER_SECOND;
			Uint64 spent_nanoseconds = SDL_GetTicksNS() - now_ticks;

			if (spent_nanoseconds < frame_nanoseconds)
			{
				SDL_DelayNS(frame_nanoseconds - spent_nanoseconds);
			}
		}
	}

	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(window_renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	scene->close();
	soft_render_destroy(&renderer);
	return 0;
}
