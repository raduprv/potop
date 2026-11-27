// damage_shot: draws frames of a damage scene to PNG files with no
// window (what Claude looks at; damage_view shows Radu the same pixels
// live).
//
//   damage_shot --out shot.png [--scene NAME] [--frame N] [--size WxH]
//               [--camera ex,ey,ez,tx,ty,tz] [--no-outlines] [--debug]
//               [--events FILE.txt]
//   damage_shot --out sheet.png --sheet FIRST,STEP,COUNT [--columns N] ...
//   damage_shot --list
//
// --sheet puts COUNT frames (FIRST, FIRST + STEP, ...) on one picture,
// each of the --size given and with its frame's number on it. --debug
// adds the scene's debug marks (contacts and their impulses). --events
// replays the balls and blasts damage_view saved (P) with a picture. The
// checksum printed is the scene's at the last frame drawn: the same
// number from damage_view (P) means the same world.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "command_line.h"
#include "damage_events.h"
#include "damage_scene.h"
#include "soft_render.h"
#include "soft_render_png.h"

#define SHEET_FRAMES_MAX 64

static void draw_frame(const damage_scene_t *scene, soft_render_t *renderer, const soft_render_camera_t *camera, int frame_index, bool draw_outlines, bool draw_debug)
{
	char caption[64];
	char about_frame[200];

	scene->seek(frame_index);
	scene->draw(renderer, camera, draw_outlines, draw_debug);
	snprintf(caption, sizeof(caption), "FRAME %d  T %.2f S", frame_index, (double)frame_index / (double)DAMAGE_SCENE_FRAMES_PER_SECOND);
	soft_render_text(renderer, 6, 6, 2, 0xFFFFFF, caption);
	scene->describe(about_frame, (int)sizeof(about_frame));
	soft_render_text(renderer, 6, 26, 1, 0xFFFFFF, about_frame);
}

static void print_scenes(FILE *file)
{
	int i;

	for (i = 0; i < damage_scene_count(); i++)
	{
		fprintf(file, "  %-8s %s\n", damage_scene_get(i)->name, damage_scene_get(i)->about);
	}
}

static int print_usage(void)
{
	fprintf(stderr,
		"usage: damage_shot --out FILE.png [--scene NAME] [--frame N] [--size WxH]\n"
		"                   [--camera ex,ey,ez,tx,ty,tz] [--no-outlines] [--debug]\n"
		"                   [--events FILE.txt]\n"
		"                   [--sheet FIRST,STEP,COUNT] [--columns N]\n"
		"       damage_shot --list\n"
		"scenes:\n");
	print_scenes(stderr);
	return 2;
}

int main(int argument_count, char **arguments)
{
	const damage_scene_t *scene = damage_scene_default();
	const char *output_path = NULL;
	int width = 960;
	int height = 540;
	int frame_index = 0;
	int sheet_first = 0;
	int sheet_step = 1;
	int sheet_count = 0;
	int column_count = 0;
	int last_frame;
	bool draw_outlines = true;
	bool draw_debug = false;
	bool camera_given = false;
	soft_render_camera_t camera;
	soft_render_camera_t given_camera;
	soft_render_t renderer;
	clock_t start_clock;
	int i;

	memset(&given_camera, 0, sizeof(given_camera));
	for (i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		if (strcmp(argument, "--no-outlines") == 0)
		{
			draw_outlines = false;
			continue;
		}
		if (strcmp(argument, "--debug") == 0)
		{
			draw_debug = true;
			continue;
		}
		if (strcmp(argument, "--list") == 0)
		{
			print_scenes(stdout);
			return 0;
		}
		if (value == NULL)
		{
			return print_usage();
		}
		i++;
		if (strcmp(argument, "--out") == 0)
		{
			output_path = value;
		}
		else if (strcmp(argument, "--scene") == 0 && damage_scene_find(value) != NULL)
		{
			scene = damage_scene_find(value);
		}
		else if (strcmp(argument, "--frame") == 0 && sscanf(value, "%d", &frame_index) == 1 && frame_index >= 0)
		{
		}
		else if (strcmp(argument, "--size") == 0 && command_line_read_size(value, &width, &height))
		{
		}
		else if (strcmp(argument, "--camera") == 0 && command_line_read_camera(value, &given_camera))
		{
			camera_given = true;
		}
		else if (strcmp(argument, "--sheet") == 0 && sscanf(value, "%d,%d,%d", &sheet_first, &sheet_step, &sheet_count) == 3
			&& sheet_first >= 0 && sheet_step >= 1 && sheet_count >= 1 && sheet_count <= SHEET_FRAMES_MAX)
		{
		}
		else if (strcmp(argument, "--events") == 0 && damage_events_load(value))
		{
		}
		else if (strcmp(argument, "--columns") == 0 && sscanf(value, "%d", &column_count) == 1 && column_count >= 1)
		{
		}
		else
		{
			fprintf(stderr, "damage_shot: bad argument '%s %s'\n", argument, value);
			return print_usage();
		}
	}
	if (output_path == NULL)
	{
		return print_usage();
	}

	// The scene's own camera, looking from and at what was asked for if
	// anything was
	scene->default_camera(&camera);
	if (camera_given)
	{
		memcpy(camera.eye, given_camera.eye, sizeof(camera.eye));
		memcpy(camera.target, given_camera.target, sizeof(camera.target));
	}
	if (!soft_render_create(&renderer, width, height))
	{
		fprintf(stderr, "damage_shot: out of memory\n");
		return 1;
	}
	if (!scene->open())
	{
		fprintf(stderr, "damage_shot: the scene '%s' cannot be made\n", scene->name);
		return 1;
	}

	start_clock = clock();
	if (sheet_count == 0)
	{
		last_frame = frame_index;
		draw_frame(scene, &renderer, &camera, frame_index, draw_outlines, draw_debug);
		if (!soft_render_save_png(&renderer, output_path))
		{
			fprintf(stderr, "damage_shot: cannot write '%s'\n", output_path);
			return 1;
		}
		printf("%s: scene %s, frame %d, %d x %d", output_path, scene->name, frame_index, width, height);
	}
	else
	{
		soft_render_sheet_t sheet;
		int row_count;

		if (column_count == 0)
		{
			// As square as it gets, wider than tall
			column_count = 1;
			while (column_count * column_count < sheet_count)
			{
				column_count++;
			}
		}
		row_count = (sheet_count + column_count - 1) / column_count;
		if (!soft_render_sheet_create(&sheet, column_count, row_count, width, height))
		{
			fprintf(stderr, "damage_shot: out of memory\n");
			return 1;
		}
		last_frame = sheet_first + (sheet_count - 1) * sheet_step;
		for (i = 0; i < sheet_count; i++)
		{
			draw_frame(scene, &renderer, &camera, sheet_first + i * sheet_step, draw_outlines, draw_debug);
			soft_render_sheet_put(&sheet, i, &renderer);
		}
		if (!soft_render_sheet_save_png(&sheet, output_path))
		{
			fprintf(stderr, "damage_shot: cannot write '%s'\n", output_path);
			return 1;
		}
		printf("%s: scene %s, %d frames from %d every %d, %d x %d each", output_path, scene->name, sheet_count, sheet_first, sheet_step, width, height);
		soft_render_sheet_destroy(&sheet);
	}
	printf(", checksum %08X at frame %d, %.0f ms\n", (unsigned int)scene->checksum(), last_frame,
		(double)(clock() - start_clock) * 1000.0 / (double)CLOCKS_PER_SEC);

	scene->close();
	soft_render_destroy(&renderer);
	return 0;
}
