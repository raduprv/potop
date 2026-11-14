// house_view: a house drawn as the game draws it, from a few sides, into
// PNG files, without anyone watching (for looking at what potop_house makes
// on a machine without a screen; Mesa's lavapipe is enough). The same
// recipe arguments as potop_house, then:
//
//   tools/house_view [recipe arguments] --out PREFIX [--views front,back,left,right,above,close,all]
//                    [--month M] [--wet W] [--detail N] [--size WxH] [--sun AZIMUTH,ELEVATION]
//
// Writes PREFIX_<view>.png for each view (--views all: every one; the
// default: front, back and close). The house stands on a square of ground
// marked in meters, in the sun, with no shadows; its weathered materials
// go through the game's prop shaders (moss, snow in the snow's months by
// --month), the rest through the object shaders.

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "house_grow.h"
#include "house_preview.h"
#include "math3d.h"
#include "post.h"
#include "render_target.h"
#include "screenshot.h"
#include "settings.h"

typedef struct
{
	SDL_Window *window;
	settings_t settings;
	render_target_t target;
	post_t post;
	house_preview_t preview;
} viewer_t;

typedef struct
{
	const char *name;
	float yaw_degrees;                     // round the house, 0: from the front (+Z), counter clockwise seen from above
	float pitch_degrees;
	float distance_scale;                  // of the house's size
	float look_height_scale;               // of its height
} view_spec_t;

static const view_spec_t view_specs[] =
{
	{ "front", 25.0f, 14.0f, 2.1f, 0.42f },
	{ "back", 205.0f, 14.0f, 2.1f, 0.42f },
	{ "left", 115.0f, 12.0f, 2.1f, 0.42f },
	{ "right", 295.0f, 12.0f, 2.1f, 0.42f },
	{ "above", 40.0f, 50.0f, 2.0f, 0.4f },
	{ "close", 35.0f, 8.0f, 0.9f, 0.3f },
	{ "street", 60.0f, 3.0f, 1.4f, 0.2f },
	{ "eave", 20.0f, 30.0f, 0.55f, 0.95f },
	{ "door", -30.0f, 5.0f, 1.0f, 0.18f },
};

static void usage(void)
{
	printf("house_view: a house drawn from a few sides into PNG files\n\n");
	printf("  recipe arguments as potop_house: --recipe FILE, --preset NAME, --seed N, --set KEY=VALUE, --data DIR\n");
	printf("  --out PREFIX           PREFIX_<view>.png (required)\n");
	printf("  --views LIST           front,back,left,right,above,close,street,eave,door or all (front,back,close)\n");
	printf("  --month M              1..12 (7): the snow's months make snow\n");
	printf("  --wet W                0..1 after the rain (0)\n");
	printf("  --detail N             the level of detail drawn, 0..3 (0)\n");
	printf("  --collision            draw the collision shell too\n");
	printf("  --hide MATERIAL        leave a material out (walls, timber, roof, base, glass)\n");
	printf("  --show WHAT            the baked weathering as gray instead of the house: sky, edge or shelter\n");
	printf("  --age A                how old the house is drawn, 0..1 (0.6)\n");
	printf("  --dirt M               how high the dirt splashes up its walls, meters (0.5)\n");
	printf("  --size WxH             the picture (1400x900)\n");
	printf("  --sun AZ,EL            the sun's azimuth and elevation (degrees; 125,42)\n");
	printf("  --look EX,EY,EZ,TX,TY,TZ  the eye and the point looked at (model meters), for every view\n");
}

int main(int argument_count, char **arguments)
{
	static viewer_t viewer;
	house_recipe_t recipe;
	house_mesh_t mesh;
	const char *data_directory = "data";
	const char *out_prefix = NULL;
	const char *views = "front,back,close";
	float month = 7.0f;
	float wetness = 0.0f;
	int detail = 0;
	bool show_collision = false;
	const char *hide = NULL;
	int show = 0;
	float age = 0.6f;
	float dirt_meters = 0.5f;
	int width = 1400;
	int height = 900;
	float sun_azimuth = 125.0f;
	// --look: the eye and the point looked at, given (every view then)
	float look[6];
	bool look_given = false;
	float sun_elevation = 42.0f;
	char views_copy[256];
	char *token;
	char *rest;

	house_recipe_defaults(&recipe);
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		bool has_next = i + 1 < argument_count;

		if (strcmp(argument, "--recipe") == 0 && has_next)
		{
			if (!house_recipe_load(&recipe, arguments[++i]))
			{
				fprintf(stderr, "house_view: %s\n", house_last_error());
				return 1;
			}
		}
		else if (strcmp(argument, "--preset") == 0 && has_next)
		{
			if (!house_preset(&recipe, arguments[++i]))
			{
				fprintf(stderr, "house_view: %s\n", house_last_error());
				return 1;
			}
		}
		else if (strcmp(argument, "--seed") == 0 && has_next)
		{
			recipe.seed = (uint32_t)strtoul(arguments[++i], NULL, 10);
		}
		else if (strcmp(argument, "--set") == 0 && has_next)
		{
			char pair[256];
			char *equals;

			snprintf(pair, sizeof(pair), "%s", arguments[++i]);
			equals = strchr(pair, '=');
			if (equals == NULL || !house_recipe_set(&recipe, (*equals = '\0', pair), equals + 1))
			{
				fprintf(stderr, "house_view: %s\n", house_last_error());
				return 1;
			}
		}
		else if (strcmp(argument, "--data") == 0 && has_next)
		{
			data_directory = arguments[++i];
		}
		else if (strcmp(argument, "--out") == 0 && has_next)
		{
			out_prefix = arguments[++i];
		}
		else if (strcmp(argument, "--views") == 0 && has_next)
		{
			views = arguments[++i];
		}
		else if (strcmp(argument, "--month") == 0 && has_next)
		{
			month = (float)atof(arguments[++i]);
		}
		else if (strcmp(argument, "--wet") == 0 && has_next)
		{
			wetness = (float)atof(arguments[++i]);
		}
		else if (strcmp(argument, "--detail") == 0 && has_next)
		{
			detail = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--collision") == 0)
		{
			show_collision = true;
		}
		else if (strcmp(argument, "--hide") == 0 && has_next)
		{
			hide = arguments[++i];
		}
		else if (strcmp(argument, "--show") == 0 && has_next)
		{
			const char *what = arguments[++i];

			show = strcmp(what, "sky") == 0 ? 1 : (strcmp(what, "edge") == 0 ? 2 : (strcmp(what, "shelter") == 0 ? 3 : 0));
		}
		else if (strcmp(argument, "--age") == 0 && has_next)
		{
			age = (float)atof(arguments[++i]);
		}
		else if (strcmp(argument, "--dirt") == 0 && has_next)
		{
			dirt_meters = (float)atof(arguments[++i]);
		}
		else if (strcmp(argument, "--size") == 0 && has_next)
		{
			sscanf(arguments[++i], "%dx%d", &width, &height);
		}
		else if (strcmp(argument, "--sun") == 0 && has_next)
		{
			sscanf(arguments[++i], "%f,%f", &sun_azimuth, &sun_elevation);
		}
		else if (strcmp(argument, "--look") == 0 && has_next)
		{
			look_given = sscanf(arguments[++i], "%f,%f,%f,%f,%f,%f", &look[0], &look[1], &look[2], &look[3], &look[4], &look[5]) == 6;
		}
		else
		{
			usage();
			return strcmp(argument, "--help") == 0 ? 0 : 1;
		}
	}
	if (out_prefix == NULL)
	{
		usage();
		return 1;
	}
	if (!house_grow(&recipe, data_directory, &mesh))
	{
		fprintf(stderr, "house_view: %s\n", house_last_error());
		return 1;
	}
	printf("%zu triangles (levels: %zu, %zu, %zu)\n", house_mesh_triangles(&mesh, 0), house_mesh_triangles(&mesh, 1), house_mesh_triangles(&mesh, 2),
		house_mesh_triangles(&mesh, 3));
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "house_view: %s\n", SDL_GetError());
		return 1;
	}
	viewer.window = SDL_CreateWindow("Potop house view", width, height, 0);
	if (viewer.window == NULL || !gpu_create(viewer.window, false, 8.0f))
	{
		fprintf(stderr, "house_view: no window or no GPU: %s\n", SDL_GetError());
		return 1;
	}
	settings_set_defaults(&viewer.settings);
	viewer.settings.render.auto_exposure = false;
	viewer.settings.render.exposure_ev = 0.0f;
	if (!post_create(&viewer.post) || !house_preview_create(&viewer.preview) || !house_preview_set_mesh(&viewer.preview, &mesh, data_directory))
	{
		fprintf(stderr, "house_view: cannot start (shaders?): %s\n", SDL_GetError());
		return 1;
	}
	snprintf(views_copy, sizeof(views_copy), "%s", strcmp(views, "all") == 0 ? "front,back,left,right,above,close,street" : views);
	rest = views_copy;
	while ((token = strtok(rest, ",")) != NULL)
	{
		const view_spec_t *spec = NULL;
		house_view_t view;
		float house_height = mesh.maximum[1];
		float house_size = SDL_max(SDL_max(mesh.maximum[0] - mesh.minimum[0], mesh.maximum[2] - mesh.minimum[2]), house_height);
		float yaw;
		float pitch;
		float distance;

		rest = NULL;
		for (size_t s = 0; s < SDL_arraysize(view_specs); s++)
		{
			if (strcmp(view_specs[s].name, token) == 0)
			{
				spec = &view_specs[s];
			}
		}
		if (spec == NULL)
		{
			fprintf(stderr, "house_view: no view '%s'\n", token);
			continue;
		}
		// The yaw: 0 looks at the front from +Z; the camera goes round
		yaw = degrees_to_radians(90.0f - spec->yaw_degrees);
		pitch = degrees_to_radians(spec->pitch_degrees);
		distance = house_size * spec->distance_scale;
		SDL_zero(view);
		view.target[0] = 0.5f * (mesh.minimum[0] + mesh.maximum[0]);
		view.target[1] = house_height * spec->look_height_scale;
		view.target[2] = 0.5f * (mesh.minimum[2] + mesh.maximum[2]);
		view.eye[0] = view.target[0] + SDL_cosf(pitch) * SDL_cosf(yaw) * distance;
		view.eye[1] = view.target[1] + SDL_sinf(pitch) * distance;
		view.eye[2] = view.target[2] + SDL_cosf(pitch) * SDL_sinf(yaw) * distance;
		if (look_given)
		{
			for (int k = 0; k < 3; k++)
			{
				view.eye[k] = look[k];
				view.target[k] = look[3 + k];
			}
		}
		view.aspect = (float)width / (float)height;
		view.sun_azimuth_degrees = sun_azimuth;
		view.sun_elevation_degrees = sun_elevation;
		view.month = month;
		view.wetness = wetness;
		view.detail = detail;
		view.show_collision = show_collision;
		view.hide = hide;
		view.show = show;
		view.age = age;
		view.dirt_meters = dirt_meters;
		view.snow_first_month = SDL_roundf(viewer.settings.render.snow_first_month);
		view.snow_last_month = SDL_roundf(viewer.settings.render.snow_last_month);
		// Two frames: the first warms the post processing up
		for (int frame = 0; frame < 2; frame++)
		{
			float sky[4];
			int scene_width = 0;
			int scene_height = 0;
			SDL_GPURenderPass *pass;
			uint8_t *pixels = NULL;
			uint32_t shot_width = 0;
			uint32_t shot_height = 0;

			gpu_begin_frame();
			house_preview_sky(sky);
			pass = render_target_begin(&viewer.target, width, height, 1.0f, sky, &scene_width, &scene_height);
			if (pass != NULL)
			{
				house_preview_draw(&viewer.preview, pass, &view);
				render_target_end(&viewer.target, pass);
				post_draw(&viewer.post, viewer.target.color_texture, scene_width, scene_height, &viewer.settings.render, 1.0, true);
			}
			gpu_end_frame(frame == 1 ? &pixels : NULL, &shot_width, &shot_height);
			if (pixels != NULL)
			{
				char path[HOUSE_PATH_LENGTH];

				snprintf(path, sizeof(path), "%s_%s.png", out_prefix, spec->name);
				if (screenshot_save(path, pixels, (int)shot_width, (int)shot_height))
				{
					printf("%s\n", path);
				}
				else
				{
					fprintf(stderr, "house_view: cannot write %s\n", path);
				}
				SDL_free(pixels);
			}
		}
	}
	house_mesh_free(&mesh);
	house_preview_destroy(&viewer.preview);
	post_destroy(&viewer.post);
	render_target_destroy(&viewer.target);
	gpu_destroy();
	SDL_DestroyWindow(viewer.window);
	SDL_Quit();
	return 0;
}
