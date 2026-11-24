// A game's application around the engine (potop.h: potop_start,
// potop_frame_begin, potop_frame_end, potop_stop): the window and the GPU,
// the settings, the world and the player's session, the HUD; each frame
// the events, the walker, the world's steps, the weapon, the picture.

#include "potop_engine.h"

#include <SDL3/SDL.h>

#include "file_io.h"
#include "screenshot.h"
#include "ui_loading.h"
#include "gpu.h"
#include "profile.h"
#include "water_gpu.h"
#include "scenes.h"

// A map's file name without its kind (.active, .baked): the world map's
// picture is looked for by it
static void map_name_shown(const char *name, char *shown_out, size_t shown_size)
{
	const char *dot;

	SDL_strlcpy(shown_out, name != NULL ? name : "", shown_size);
	dot = SDL_strrchr(shown_out, '.');
	if (dot != NULL && (SDL_strcmp(dot, ".active") == 0 || SDL_strcmp(dot, ".baked") == 0))
	{
		shown_out[dot - shown_out] = '\0';
	}
}

static bool create_window(potop_t *potop, const potop_options_t *options)
{
	const render_settings_t *render = &potop->settings.render;
	bool fullscreen = render->start_fullscreen;
	bool maximized = !fullscreen && render->start_maximized;
	int width = options->width > 0 ? options->width : 1280;
	int height = options->height > 0 ? options->height : 720;

	// Fullscreen from the start: as big as the screen right away, so that it
	// never changes size once the rendering has begun
	if (fullscreen)
	{
		const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());

		if (mode != NULL)
		{
			width = mode->w;
			height = mode->h;
		}
	}
	potop->window = SDL_CreateWindow(options->title != NULL ? options->title : "Potop", width, height,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0) | (maximized ? SDL_WINDOW_MAXIMIZED : 0));
	if (potop->window == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	if (fullscreen || maximized)
	{
		SDL_SyncWindow(potop->window);
	}
	if (!gpu_create(potop->window, render->vsync, render->anisotropy))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No GPU driver can run the game (Vulkan, Metal or Direct3D 12 needed)");
		return false;
	}
	potop->gpu_ready = true;
	if (render->shadow_depth_16_bit
		&& SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
	{
		gpu.shadow_depth_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
	}
	gpu_set_scene_samples(render->msaa);
	return true;
}

// While the world is made: the loading screen (world_t.progress), and the
// stages' times to the log
static void startup_progress(void *user, const char *stage, float fraction)
{
	potop_t *potop = user;
	uint64_t now = SDL_GetPerformanceCounter();

	if (potop->stage_name != NULL)
	{
		SDL_Log("Startup: %s: %.2f s", potop->stage_name, (double)(now - potop->stage_counter) / (double)SDL_GetPerformanceFrequency());
	}
	potop->stage_name = stage;
	if (potop->ui_ready)
	{
		ui_loading_frame(&potop->backend, potop->title, stage, fraction);
	}
	potop->stage_counter = SDL_GetPerformanceCounter();
}

// The houses' gallery: every object type of the category (none given:
// "buildings", or failing that "fable"), or the types named (comma
// separated: "--houses cow_farm,stana", a scene of a few things to try
// something on), in rows north of the middle of the flat ground, the
// player standing in the middle
#define GALLERY_ROW_WIDTH_METERS 150.0f
#define GALLERY_GAP_METERS 4.0f
#define GALLERY_FIRST_ROW_METERS 8.0f

// How far round a building its turning parts reach (types that spin named
// after it, windmill_sails by windmill), at their size: the gallery keeps
// its neighbors out of their way
static float gallery_parts_radius(const objects_t *objects, const object_type_t *type)
{
	const object_types_t *types = &objects->types;
	size_t name_length = SDL_strlen(type->name);
	float radius = 0.0f;

	for (int p = 0; p < types->type_count; p++)
	{
		const object_type_t *part = &types->types[p];

		if (part->spins && part->model >= 0 && SDL_strncmp(part->name, type->name, name_length) == 0 && part->name[name_length] == '_')
		{
			// (it turns: as far as any of it is from its origin)
			radius = SDL_max(radius, objects->models[part->model].horizontal_radius * 0.5f * (part->scale_minimum + part->scale_maximum));
		}
		// (a module's part: as far as it reaches from the module's origin,
		// however it turns)
		if (part->module_type >= 0 && part->model >= 0 && &types->types[part->module_type] == type && type->module >= 0
			&& part->module_part < types->modules[type->module].part_count)
		{
			const float *at = types->modules[type->module].parts[part->module_part].position;
			float reach = SDL_sqrtf(at[0] * at[0] + at[2] * at[2]) + objects->models[part->model].horizontal_radius;

			radius = SDL_max(radius, reach * 0.5f * (type->scale_minimum + type->scale_maximum));
		}
	}
	return radius;
}

// The gallery's packing: each type its own rectangle (its model's bounds
// turned round as it stands, at its size, out to its turning parts' reach,
// a gap round it), laid bottom left on a skyline across the rows' width:
// each where the skyline under it is lowest (a big farm no longer makes
// its whole row as deep as itself, nor its bounding circle round an origin
// at one end of it wastes the rest). Gives the depth of it all and how
// wide it came out (width_out); at (x, z) for each type (NaN: not laid),
// its origin's place from the rows' start
#define GALLERY_SKYLINE_CELL_METERS 0.5f

static float house_gallery_pack(const objects_t *objects, const bool *chosen, float *at_x, float *at_z, float *width_out)
{
	const object_types_t *types = &objects->types;
	enum { CELLS = (int)(GALLERY_ROW_WIDTH_METERS / GALLERY_SKYLINE_CELL_METERS) };
	float skyline[CELLS];
	float depth = 0.0f;

	*width_out = 0.0f;
	for (int c = 0; c < CELLS; c++)
	{
		skyline[c] = 0.0f;
	}
	for (int t = 0; t < types->type_count; t++)
	{
		const object_type_t *type = &types->types[t];
		const object_model_t *model;
		float scale;
		float parts;
		float x0;
		float x1;
		float z0;
		float z1;
		int width_cells;
		int best = 0;
		float best_height = 1e30f;

		at_x[t] = NAN;
		at_z[t] = NAN;
		if (type->model < 0 || !chosen[t] || type->spins || type->module_type >= 0)
		{
			continue;
		}
		model = &objects->models[type->model];
		scale = 0.5f * (type->scale_minimum + type->scale_maximum);
		// (turned round, a half turn: x and z both the other way)
		x0 = -model->bounds_maximum.x * scale;
		x1 = -model->bounds_minimum.x * scale;
		z0 = -model->bounds_maximum.z * scale;
		z1 = -model->bounds_minimum.z * scale;
		parts = gallery_parts_radius(objects, type);
		x0 = SDL_min(x0, -parts) - GALLERY_GAP_METERS * 0.5f;
		x1 = SDL_max(x1, parts) + GALLERY_GAP_METERS * 0.5f;
		z0 = SDL_min(z0, -parts) - GALLERY_GAP_METERS * 0.5f;
		z1 = SDL_max(z1, parts) + GALLERY_GAP_METERS * 0.5f;
		width_cells = (int)SDL_ceilf((x1 - x0) / GALLERY_SKYLINE_CELL_METERS);
		width_cells = SDL_min(SDL_max(width_cells, 1), CELLS);
		for (int c = 0; c + width_cells <= CELLS; c++)
		{
			float height = 0.0f;

			for (int k = c; k < c + width_cells; k++)
			{
				height = SDL_max(height, skyline[k]);
			}
			if (height < best_height - 0.01f)
			{
				best_height = height;
				best = c;
			}
		}
		at_x[t] = (float)best * GALLERY_SKYLINE_CELL_METERS - x0;
		at_z[t] = best_height - z0;
		for (int k = best; k < best + width_cells; k++)
		{
			skyline[k] = best_height + (z1 - z0);
		}
		depth = SDL_max(depth, best_height + (z1 - z0));
		*width_out = SDL_max(*width_out, (float)(best + width_cells) * GALLERY_SKYLINE_CELL_METERS);
	}
	return depth;
}

// Returns where the rows start (z, meters): the player is set in front of them
// The objects a test puts down besides the gallery (potop_options_t.put_types)
static void put_objects(potop_t *potop, const potop_options_t *options)
{
	objects_t *objects = &potop->world.objects;

	for (int k = 0; k < options->put_count; k++)
	{
		int type = object_types_find(&objects->types, options->put_types[k]);
		const char *at = options->put_points[k];
		int placed = 0;

		if (type < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "--put: no type '%s' in objects.ini", options->put_types[k]);
			continue;
		}
		while (at != NULL && *at != '\0')
		{
			char *end = NULL;
			float x = (float)SDL_strtod(at, &end);
			float z;

			if (end == at || *end != ',')
			{
				break;
			}
			at = end + 1;
			z = (float)SDL_strtod(at, &end);
			if (end == at)
			{
				break;
			}
			placed += objects_add(objects, &potop->world.terrain, type, x, z, 0.0f,
				0.5f * (objects->types.types[type].scale_minimum + objects->types.types[type].scale_maximum), 0) ? 1 : 0;
			at = *end == ';' ? end + 1 : NULL;
		}
		SDL_Log("Put: %d of '%s'", placed, options->put_types[k]);
	}
}

static float place_house_gallery(potop_t *potop, const char *category_name)
{
	objects_t *objects = &potop->world.objects;
	const object_types_t *types = &objects->types;
	float map_meters = (float)potop->world.terrain.size * potop->world.terrain.cell_size_meters;
	float middle = map_meters * 0.5f;
	float start_x = middle - GALLERY_ROW_WIDTH_METERS * 0.5f;
	float z = middle + GALLERY_FIRST_ROW_METERS;
	float depth;
	float start_z;
	float *at_x;
	float *at_z;
	bool *chosen;
	int chosen_count = 0;
	int placed = 0;
	int category = object_types_find_category(types, category_name[0] != '\0' ? category_name : "buildings");

	if (category < 0 && category_name[0] == '\0')
	{
		category = object_types_find_category(types, "fable");
	}
	at_x = SDL_malloc(sizeof(float) * (size_t)SDL_max(types->type_count, 1));
	at_z = SDL_malloc(sizeof(float) * (size_t)SDL_max(types->type_count, 1));
	chosen = SDL_calloc((size_t)SDL_max(types->type_count, 1), sizeof(bool));
	if (at_x == NULL || at_z == NULL || chosen == NULL)
	{
		SDL_free(at_x);
		SDL_free(at_z);
		SDL_free(chosen);
		return z;
	}
	// The category's types, or else the types named
	for (int t = 0; t < types->type_count; t++)
	{
		if (category >= 0)
		{
			chosen[t] = types->types[t].category == category;
		}
		else
		{
			const char *name = types->types[t].name;
			size_t length = SDL_strlen(name);
			const char *at = category_name;

			while (*at != '\0' && !chosen[t])
			{
				const char *end = SDL_strchr(at, ',');
				size_t word = end != NULL ? (size_t)(end - at) : SDL_strlen(at);

				chosen[t] = word == length && SDL_strncmp(at, name, length) == 0;
				at = end != NULL ? end + 1 : at + word;
			}
		}
		chosen_count += chosen[t] ? 1 : 0;
	}
	if (chosen_count == 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Houses: no category nor types \"%s\" in objects.ini",
			category_name[0] != '\0' ? category_name : "buildings");
		SDL_free(at_x);
		SDL_free(at_z);
		SDL_free(chosen);
		return z;
	}
	// (the rows start in front of the player, but early enough that the
	// last of them still fits: they fell off the map's edge as more houses
	// came; a margin of a few meters kept)
	{
		float width;

		depth = house_gallery_pack(objects, chosen, at_x, at_z, &width);
		// (a narrower lot centered in front of the player: a few things)
		start_x = middle - width * 0.5f;
	}
	z = SDL_min(z, map_meters - GALLERY_GAP_METERS - depth);
	z = SDL_max(z, GALLERY_GAP_METERS);
	start_z = z;
	if (z + depth > map_meters)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Houses: the rows are %.0f m deep, the map only %.0f m: the last ones are left out",
			(double)depth, (double)map_meters);
	}
	for (int t = 0; t < types->type_count; t++)
	{
		const object_type_t *type = &types->types[t];
		float scale;
		float x;
		float y;

		// (what turns stands with its building: below, not on its own)
		if (!chosen[t] || type->spins || type->module_type >= 0)
		{
			continue;
		}
		if (type->model < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Houses: '%s' has no model (its file did not load), left out", type->name);
			continue;
		}
		scale = 0.5f * (type->scale_minimum + type->scale_maximum);
		x = start_x + at_x[t];
		y = start_z + at_z[t];
		// (turned round: their fronts, +Z, toward the player in the middle)
		SDL_Log("Houses: '%s' at (%.2f, %.2f), turned a half turn (its model's +X to -X, +Z to -Z)", type->name, (double)x, (double)y);
		if (!objects_add(objects, &potop->world.terrain, t, x, y, SDL_PI_F, scale, 0))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Houses: '%s' not placed at (%.1f, %.1f) m%s (the map is %.0f m)", type->name, (double)x, (double)y,
				x >= map_meters || y >= map_meters || x < 0.0f || y < 0.0f ? ", outside the map" : "", (double)map_meters);
		}
		// (its turning parts, as its layout puts them: a type that spins
		// and is named after it, windmill_sails by windmill, on the same
		// spot, turned the same way; the layout's own placing levels the
		// ground and turns it as it likes)
		{
			size_t name_length = SDL_strlen(type->name);

			for (int p = 0; p < types->type_count; p++)
			{
				const object_type_t *part = &types->types[p];

				if (part->spins && part->model >= 0 && SDL_strncmp(part->name, type->name, name_length) == 0 && part->name[name_length] == '_')
				{
					objects_add(objects, &potop->world.terrain, p, x, y, SDL_PI_F, scale, 0);
				}
			}
		}
		placed++;
	}
	SDL_free(at_x);
	SDL_free(at_z);
	SDL_free(chosen);
	SDL_Log("Houses: %d of %s, rows %.0f m deep (the map is %.0f m)", placed, category >= 0 ? types->categories[category] : category_name,
		(double)depth, (double)map_meters);
	return start_z;
}

bool potop_start(potop_t *potop, const potop_options_t *options)
{
	potop_options_t defaults;
	uint32_t seed;
	float gallery_start_z = 0.0f;

	if (options == NULL)
	{
		SDL_zero(defaults);
		options = &defaults;
	}
	SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
	// (the console keeps the log from the start)
	console_create(&potop->console);
	potop->console_ready = true;
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
		return false;
	}
	potop->sdl_ready = true;
	file_io_initialize();
	settings_set_defaults(&potop->settings);
	settings_load(&potop->settings);
	potop->file_settings = potop->settings;
	settings_load_overrides(&potop->settings);
	for (int i = 0; i < options->settings_set_count; i++)
	{
		if (!settings_set_from_text(&potop->settings, options->settings_set[i]))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "--set %s: no such setting (section.key=value)", options->settings_set[i]);
		}
	}
	potop->sim_seconds = options->sim_seconds;
	potop->sim_step = options->sim_step > 0.0f ? options->sim_step : 0.05f;
	if (potop->sim_seconds > 0.0f)
	{
		// (nothing waits for the screen, nothing is seen from afar)
		potop->settings.render.vsync = false;
		potop->settings.objects.impostors = false;
	}
	soils_set_defaults(&potop->soils);
	soils_load(&potop->soils);
	world_init(&potop->world, &potop->settings, &potop->soils);
	if (!create_window(potop, options))
	{
		return false;
	}
	hud_init(&potop->hud);
	ui_style_defaults(&potop->style);
	ui_style_load(&potop->style);
	potop->ui_ready = ui_backend_create(&potop->backend, potop->window, potop->style.font_path, potop->style.font_size,
		potop->style.dialog_text_scale, potop->style.scale);
	if (potop->ui_ready)
	{
		ui_style_apply(&potop->style, &potop->backend.context);
	}
	else
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No interface: the HUD is not shown");
	}
	settings_window_init(&potop->settings_window, &potop->style);
	SDL_strlcpy(potop->title, options->title != NULL ? options->title : "Potop", sizeof(potop->title));
	potop->world.progress = startup_progress;
	potop->world.progress_user = potop;
	startup_progress(potop, "Making the land", 0.04f);
	if (options->houses != NULL)
	{
		// The houses' gallery: small flat ground, no map
		if (!world_create_land(&potop->world, SCENE_FLAT_SIZE, SCENE_FLAT_CELL_SIZE_METERS, false))
		{
			return false;
		}
		scenes_build_flat(&potop->world.terrain);
	}
	else if (!world_create_land(&potop->world, potop->settings.terrain.size, SDL_max(potop->settings.terrain.cell_size_meters, 0.01f), false))
	{
		return false;
	}
	seed = options->seed != 0 ? options->seed : potop->settings.terrain.seed;
	if (seed == 0)
	{
		seed = (uint32_t)SDL_GetPerformanceCounter() | 1u;
	}
	if (options->map == NULL && options->houses == NULL && !world_generate(&potop->world, seed))
	{
		return false;
	}
	if (!world_create(&potop->world))
	{
		return false;
	}
	// (a game's animals walk all the time: their paths not to the log, as
	// the editor's tests want them)
	potop->world.actors.log_paths = options->sim_seconds > 0.0f;
	session_create(&potop->session, &potop->world);
	camera_reset(&potop->camera, &potop->settings.camera, &potop->world.terrain);
	if (options->houses != NULL)
	{
		gallery_start_z = place_house_gallery(potop, options->houses);
		put_objects(potop, options);
		world_populate(&potop->world, false);
	}
	else if (options->map != NULL)
	{
		if (potop_map_load(potop, options->map) == POTOP_LOAD_FAILED)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load the map \"%s\"", options->map);
			return false;
		}
	}
	else
	{
		world_populate(&potop->world, potop->settings.objects.populate);
	}
	startup_progress(potop, "Getting ready", 0.99f);
	potop->world.progress = NULL;
	potop->stage_name = NULL;
	// A game is on foot
	camera_set_first_person(&potop->camera, &potop->settings.camera, &potop->world.terrain, true);
	// (the houses' gallery: looking at its first rows)
	if (options->houses != NULL)
	{
		float middle = (float)potop->world.terrain.size * potop->world.terrain.cell_size_meters * 0.5f;

		// (in front of the first row, which starts earlier when there are
		// many, so the last still fit on the map)
		camera_go_to(&potop->camera, &potop->world.terrain, middle, NAN, gallery_start_z - GALLERY_FIRST_ROW_METERS);
		camera_look_at(&potop->camera, &potop->settings.camera, middle, potop->camera.walker_feet + 4.0f, gallery_start_z + 25.0f);
	}
	// A test without the picture: no HUD either (its frames are never
	// drawn, so never cleared)
	if (potop->sim_seconds > 0.0f && potop->ui_ready)
	{
		ui_backend_destroy(&potop->backend);
		potop->ui_ready = false;
	}
	if (options->start_hour_set)
	{
		potop->world.day_hour = SDL_fmod((double)options->start_hour, 24.0);
	}
	potop->bake_paths_frames = options->bake_paths ? 3 : 0;
	// (--push: "X,Z,IX,IZ@SECONDS")
	potop->push_stage = 0;
	if (options->push != NULL)
	{
		const char *at = options->push;
		char *end = NULL;

		for (int k = 0; k < 4; k++)
		{
			potop->push_values[k] = (float)SDL_strtod(at, &end);
			at = *end == ',' ? end + 1 : end;
		}
		potop->push_seconds = *end == '@' ? (float)SDL_strtod(end + 1, &end) : 0.0f;
		potop->push_stage = 1;
	}
	// (--take: "X,Z@SECONDS;...", in the order of their times)
	potop->take_count = 0;
	potop->take_done = 0;
	for (const char *at = options->take; at != NULL && *at != '\0' && potop->take_count < POTOP_TAKE_MAX;)
	{
		char *end;
		float x = (float)SDL_strtod(at, &end);
		float z = *end == ',' ? (float)SDL_strtod(end + 1, &end) : 0.0f;
		float seconds = *end == '@' ? (float)SDL_strtod(end + 1, &end) : 0.0f;

		potop->take_points[potop->take_count][0] = x;
		potop->take_points[potop->take_count][1] = z;
		potop->take_seconds[potop->take_count++] = seconds;
		at = *end == ';' ? end + 1 : NULL;
	}
	input_initialize(&potop->input);
	potop->previous_counter = SDL_GetPerformanceCounter();
	return true;
}

double potop_elapsed_seconds(const potop_t *potop)
{
	return potop->world.elapsed_seconds;
}

float potop_day_hour(const potop_t *potop)
{
	return (float)potop->world.day_hour;
}

void potop_stop(potop_t *potop)
{
	if (potop->gpu_ready)
	{
		if (potop->ui_ready)
		{
			ui_backend_destroy(&potop->backend);
			potop->ui_ready = false;
		}
		session_destroy(&potop->session, &potop->world);
	}
	world_destroy(&potop->world);
	if (potop->gpu_ready)
	{
		gpu_destroy();
		potop->gpu_ready = false;
	}
	if (potop->window != NULL)
	{
		SDL_DestroyWindow(potop->window);
		potop->window = NULL;
	}
	if (potop->console_ready)
	{
		console_destroy(&potop->console);
		potop->console_ready = false;
	}
	if (potop->sdl_ready)
	{
		SDL_Quit();
		potop->sdl_ready = false;
	}
}

// The pointer is out for the HUD (the eye stays put): a window open, a
// conversation, or Tab
static bool pointer_free(const potop_t *potop)
{
	const hud_t *hud = &potop->hud;

	return potop->camera.first_person
		&& (potop->mouse_free || hud->inventory_open || hud->loot_container != 0 || hud->map_open || hud->journal_open || potop->session.dialog.open
			|| potop->settings_window.open || potop->console.open);
}

static void read_events(potop_t *potop)
{
	input_state_t *input = &potop->input;
	bool free_pointer = pointer_free(potop);
	SDL_Event event;

	if (SDL_GetWindowRelativeMouseMode(potop->window) != (potop->camera.first_person && !free_pointer))
	{
		SDL_SetWindowRelativeMouseMode(potop->window, potop->camera.first_person && !free_pointer);
	}
	input->first_person = potop->camera.first_person;
	input_begin_frame(input);
	if (potop->ui_ready)
	{
		ui_backend_input_begin(&potop->backend);
	}
	while (SDL_PollEvent(&event))
	{
		bool mouse_event = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			|| event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL;

		// The console before everything: ` (~ with shift) opens it, and
		// while open it has the keyboard
		if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_GRAVE && !potop->console.open)
		{
			if (!event.key.repeat)
			{
				console_set_open(&potop->console, true);
			}
			continue;
		}
		if (console_handle_event(&potop->console, &event))
		{
			continue;
		}
		if (event.type == SDL_EVENT_TEXT_INPUT && (SDL_strcmp(event.text.text, "`") == 0 || SDL_strcmp(event.text.text, "~") == 0))
		{
			continue;
		}
		// Tab lets the pointer out for the HUD (the keys still walk) and
		// takes it back
		if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB && potop->camera.first_person
			&& !(potop->ui_ready && potop->backend.keyboard_captured))
		{
			if (!event.key.repeat)
			{
				potop->mouse_free = !potop->mouse_free;
			}
			continue;
		}
		if (potop->camera.first_person && mouse_event && !free_pointer)
		{
			input_handle_event(input, &event);
			// (a release reaches the HUD too: a click that closed a window)
			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && potop->ui_ready)
			{
				ui_backend_handle_event(&potop->backend, &event);
			}
		}
		else if (!potop->ui_ready || !ui_backend_handle_event(&potop->backend, &event))
		{
			if (free_pointer && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			{
				// A click on the world takes the pointer back
				potop->mouse_free = false;
				continue;
			}
			input_handle_event(input, &event);
		}
		else if (event.type == SDL_EVENT_QUIT)
		{
			input->request_quit = true;
		}
	}
	if (potop->ui_ready)
	{
		ui_backend_input_end(&potop->backend);
	}
	input->keyboard_captured = potop->console.open || (potop->ui_ready && potop->backend.keyboard_captured);
	input->pointer_over_ui = potop->ui_ready && potop->backend.mouse_captured;
	input_end_frame(input);
	if (free_pointer)
	{
		input->look_x_pixels = 0.0f;
		input->look_y_pixels = 0.0f;
	}
}

// Alt: what the crosshair (first person) or the pointer is on,
// highlighted, its type named under the compass (and in the log) when Alt
// goes down and each time it is on another (to tell which object a fault
// is on)
static void update_look_pick(potop_t *potop, int pixel_width, int pixel_height)
{
	static const float color[3] = { 1.0f, 0.85f, 0.35f };
	const input_state_t *input = &potop->input;
	vec3_t origin;
	vec3_t direction;
	object_ray_hit_t hit;
	int hits;
	int instance = -1;
	char text[160];

	if (!input->pick_held || input->keyboard_captured)
	{
		if (potop->look_pick_instance != -2)
		{
			objects_set_highlight(&potop->world.objects, -1, NULL);
		}
		potop->look_pick_instance = -2;
		return;
	}
	{
		int window_width = 1;
		int window_height = 1;
		float pixel_x = (float)pixel_width * 0.5f;
		float pixel_y = (float)pixel_height * 0.5f;

		SDL_GetWindowSize(potop->window, &window_width, &window_height);
		if (!potop->camera.first_person)
		{
			pixel_x = input->pointer_x * (float)pixel_width / (float)SDL_max(window_width, 1);
			pixel_y = input->pointer_y * (float)pixel_height / (float)SDL_max(window_height, 1);
		}
		camera_pixel_ray(&potop->camera, pixel_x, pixel_y, pixel_width, pixel_height, &origin, &direction);
	}
	hits = objects_raycast(&potop->world.objects, origin, direction, potop->camera.far_distance, OBJECTS_RAY_ALL, &hit, 1);
	if (hits > 0)
	{
		instance = hit.instance;
	}
	objects_set_highlight(&potop->world.objects, instance, color);
	if (instance == potop->look_pick_instance)
	{
		return;
	}
	potop->look_pick_instance = instance;
	if (instance >= 0)
	{
		const object_instance_t *object = &potop->world.objects.instances[instance];
		const object_type_t *type = &potop->world.objects.types.types[object->type];

		SDL_snprintf(text, sizeof(text), "%s (model %s) #%u, %.1f m", type->name, type->model_path, object->id, (double)hit.distance);
	}
	else
	{
		SDL_strlcpy(text, "nothing (the ground or the sky)", sizeof(text));
	}
	potop_notify(potop, text, 4.0f, POTOP_NOTIFY_INFO);
	SDL_Log("Alt: %s", text);
}

bool potop_frame_begin(potop_t *potop, potop_input_t *input_out, float *seconds_out)
{
	input_state_t *input = &potop->input;
	uint64_t now = SDL_GetPerformanceCounter();
	double seconds = (double)(now - potop->previous_counter) / (double)SDL_GetPerformanceFrequency();

	trace_frame_begin();
	// (what the game did after the last potop_frame_end)
	if (potop->game_counter != 0)
	{
		frame_profile_end(FRAME_GAME, potop->game_counter);
	}
	potop->previous_counter = now;
	// After a stall (the window dragged) no catching up on seconds of it
	seconds = SDL_min(seconds, 0.25);
	// A test without the picture: its own steps, over after its seconds
	if (potop->sim_seconds > 0.0f)
	{
		if (potop->sim_elapsed >= (double)potop->sim_seconds)
		{
			return false;
		}
		seconds = (double)potop->sim_step;
		potop->sim_elapsed += seconds;
	}
	potop->frame_seconds = (float)seconds;
	world_begin_frame(&potop->world, seconds);
	{
		uint64_t events_start = SDL_GetPerformanceCounter();

		read_events(potop);
		frame_profile_end(FRAME_EVENTS, events_start);
	}
	trace_mark("events");
	SDL_zerop(input_out);
	input_out->select = -1;
	if (input->request_quit)
	{
		return false;
	}
	// F12: a screenshot of this frame (saved as it is shown, see
	// potop_frame_end); F11: fullscreen, borderless at the desktop's
	// resolution (no mode change; the swapchain follows the new size)
	if (input->request_screenshot)
	{
		potop->screenshot_requested = true;
	}
	// F4: the navigation mesh shown in the world, one class of walkers
	// after the other (the widest first), then not
	if (input->request_diagnostics)
	{
		const navigation_t *navigation = &potop->world.navigation;
		int next = potop->navmesh_overlay_class < 0 ? NAVIGATION_MESH_CLASSES - 1 : potop->navmesh_overlay_class - 1;
		char text[160];

		while (next >= 0 && !navigation_mesh_class_used(navigation, next))
		{
			next--;
		}
		if (next < 0 && potop->navmesh_overlay_class < 0)
		{
			SDL_snprintf(text, sizeof(text), "Navigation mesh: none yet (no walker has looked for a way)");
		}
		else if (next < 0)
		{
			SDL_snprintf(text, sizeof(text), "Navigation mesh: off");
		}
		else
		{
			SDL_snprintf(text, sizeof(text), "Navigation mesh: walkers %d quarter meter%s wide%s (ground green and blue, objects' tops yellow to "
				"purple by height; portals white, edges to jump pink)", next + 1, next == 0 ? "" : "s", next == NAVIGATION_MESH_CLASSES - 1 ? " and more" : "");
		}
		potop->navmesh_overlay_class = next;
		hud_notify(&potop->hud, text, 5.0f, HUD_NOTIFY_INFO);
	}
	if (input->toggle_fullscreen)
	{
		bool fullscreen = (SDL_GetWindowFlags(potop->window) & SDL_WINDOW_FULLSCREEN) != 0;

		SDL_SetWindowFullscreen(potop->window, !fullscreen);
	}
	// Escape: the conversation or the topmost window closed; with nothing
	// open, the game's
	if (input->request_escape && potop->settings_window.open)
	{
		potop->settings_window.open = false;
	}
	else if (input->request_escape && !hud_close_windows(&potop->hud, potop->ui_ready))
	{
		input_out->escape = true;
	}
	if (input->select_tool >= 0 && potop->session.dialog.open)
	{
		// Talking: 1..9 answer
		dialog_choose(&potop->session.dialog, input->select_tool);
	}
	else
	{
		input_out->select = input->select_tool;
	}
	if (!input->keyboard_captured)
	{
		input_out->inventory = input->toggle_inventory;
		input_out->journal = input->cycle_actor_action != 0;
		input_out->map = input->toggle_water_model;
		input_out->reload = input->toggle_rain;
		input_out->debug = input->toggle_tile_overlay;
		input_out->throw_primary = input->cycle_plant;
		input_out->throw_secondary = input->select_clear;
		input_out->place_primary = input->place_object;
		input_out->place_secondary = input->remove_object;
	}
	input_out->interact = input->interact && potop->camera.first_person;
	input_out->trigger = input->apply_tool_held && !input->pointer_over_ui && !potop->session.dialog.open;
	input_out->aim = input->right_button_down && !input->pointer_over_ui && !potop->session.dialog.open;
	input_out->pointer_free = potop->mouse_free;
	input_out->map_loaded = potop->map_loaded;
	potop->map_loaded = false;
	*seconds_out = potop->frame_seconds;
	potop->game_counter = SDL_GetPerformanceCounter();
	return true;
}

// What the HUD shows this frame, as the session, the world and the camera
// have it (the names over the actors near the eye, in front)
static void fill_hud_view(potop_t *potop, hud_view_t *view)
{
	const camera_t *camera = &potop->camera;
	const actors_t *actors = &potop->world.actors;

	SDL_zerop(view);
	view->inventories = &potop->session.inventories;
	view->player_inventory = potop->session.player_inventory;
	view->interaction_prompt = potop->session.interaction_prompt;
	view->on_foot = camera->first_person;
	view->eye_x = camera->eye_position.x;
	view->eye_z = camera->eye_position.z;
	view->heading_degrees = SDL_atan2f(-camera->view.m[2], camera->view.m[10]) * 180.0f / SDL_PI_F;
	view->markers = &potop->session.markers;
	view->journal = &potop->session.journal;
	view->dialog = &potop->session.dialog;
	view->map_texture = potop->session.world_map.texture;
	view->map_texture_width = potop->session.world_map.width;
	view->map_texture_height = potop->session.world_map.height;
	view->map_extent_meters = (float)potop->world.terrain.size * potop->world.terrain.cell_size_meters;
	view->map_from_artist = potop->session.world_map.from_artist;
	for (int a = 0; camera->first_person && a < actors->count && view->nameplate_count < HUD_NAMEPLATES_MAX; a++)
	{
		const actor_t *actor = &actors->actors[a];
		float dx = actor->x - camera->eye_position.x;
		float dz = actor->z - camera->eye_position.z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);
		const actor_model_t *model;
		vec4_t top;
		vec4_t clip;
		hud_nameplate_t *plate;

		if (actor->name[0] == '\0' || distance > 30.0f || actors_is_dead(actors, actor->id) || actors->types[actor->type].model < 0)
		{
			continue;
		}
		model = &actors->models[actors->types[actor->type].model];
		top.x = actor->x;
		top.y = actor->y - actor->root_lift + (model->bounds_maximum.y + 0.35f) * actor->scale;
		top.z = actor->z;
		top.w = 1.0f;
		clip = mat4_transform(camera->view_projection, top);
		if (clip.w <= 0.1f)
		{
			continue;
		}
		plate = &view->nameplates[view->nameplate_count++];
		plate->screen_x = clip.x / clip.w;
		plate->screen_y = clip.y / clip.w;
		plate->alpha = SDL_clamp((30.0f - distance) / 8.0f, 0.0f, 1.0f);
		// (as the depth buffer has it: the clip depth over w, as
		// objects_cull.comp compares it)
		plate->depth = clip.z / clip.w;
		plate->hostile = actor->hostile;
		SDL_strlcpy(plate->name, actor->name, sizeof(plate->name));
	}
}

// What the player asked of the HUD: drop from the pack, a marker of their
// own on the map (or the one there away)
static void handle_hud_requests(potop_t *potop)
{
	const hud_requests_t *requests = &potop->hud.requests;
	camera_t *camera = &potop->camera;

	if (requests->drop_item >= 0 && camera->first_person)
	{
		float yaw = camera->walker_yaw_degrees * SDL_PI_F / 180.0f;

		if (ground_items_drop(&potop->world.ground_items, &potop->world.objects, &potop->world.terrain, &potop->session.inventories,
				potop->session.player_inventory, requests->drop_item, requests->drop_count, camera->walker_x - SDL_sinf(yaw),
				camera->walker_z - SDL_cosf(yaw), yaw) == 0)
		{
			hud_notify(&potop->hud, "Nothing to lay it down as (objects.ini needs a sack or a crate)", 4.0f, HUD_NOTIFY_WARNING);
		}
	}
	if (requests->map_marker)
	{
		float reach = 0.02f * (float)potop->world.terrain.size * potop->world.terrain.cell_size_meters / SDL_max(potop->hud.map_zoom, 1.0f);
		uint32_t near = markers_nearest(&potop->session.markers, requests->map_marker_x, requests->map_marker_z, reach);
		marker_t *marker = markers_get(&potop->session.markers, near);

		if (marker != NULL && marker->custom)
		{
			markers_remove(&potop->session.markers, near);
		}
		else
		{
			static const float yellow[3] = { 1.0f, 0.86f, 0.25f };
			uint32_t id = markers_add(&potop->session.markers, requests->map_marker_x, requests->map_marker_z, yellow, "Waypoint");
			marker_t *added = markers_get(&potop->session.markers, id);

			if (added != NULL)
			{
				// (followed now: no quest objective in focus)
				added->custom = true;
				markers_set_active(&potop->session.markers, id);
				potop->session.journal.focus_quest[0] = '\0';
				potop->session.journal.focus_objective[0] = '\0';
			}
		}
	}
}

// A script of console commands (the environment's POTOP_SCRIPT names its
// file: one command a line, "wait N" waits N frames), run as if typed: to
// look at things the same way every time, unattended (a camera path and
// its screenshots, then quit)
static bool script_next(char *command, size_t size)
{
	static char *text = NULL;
	static size_t at = 0;
	static bool tried = false;
	static int wait_frames = 0;

	if (!tried)
	{
		const char *path = SDL_getenv("POTOP_SCRIPT");

		tried = true;
		text = path != NULL ? (char *)SDL_LoadFile(path, NULL) : NULL;
		if (path != NULL)
		{
			SDL_Log(text != NULL ? "Script: running '%s'" : "Script: cannot read '%s'", path);
		}
	}
	if (text == NULL || wait_frames-- > 0)
	{
		return false;
	}
	while (text[at] != '\0')
	{
		size_t end = at;
		size_t length;

		while (text[end] != '\0' && text[end] != '\n')
		{
			end++;
		}
		length = SDL_min(end - at, size - 1);
		SDL_memcpy(command, text + at, length);
		command[length] = '\0';
		at = text[end] == '\n' ? end + 1 : end;
		if (length > 0 && command[length - 1] == '\r')
		{
			command[--length] = '\0';
		}
		if (length == 0 || command[0] == '#')
		{
			continue;
		}
		if (SDL_strncmp(command, "wait", 4) == 0)
		{
			wait_frames = SDL_atoi(command + 4);
			return false;
		}
		return true;
	}
	return false;
}

// The console (over everything, while open) and the commands typed into it
static void run_console(potop_t *potop)
{
	char command[CONSOLE_INPUT_LENGTH];

	if (potop->ui_ready)
	{
		console_draw(&potop->console, &potop->backend, &potop->style);
		ui_backend_update_capture(&potop->backend);
	}
	while (script_next(command, sizeof(command)) || console_take_command(&potop->console, command, sizeof(command)))
	{
		console_actions_t actions;

		console_run_command(&potop->console, potop, command, &actions);
		if (actions.apply_render)
		{
			world_apply_render_settings(&potop->world);
		}
		if (actions.load_map)
		{
			potop_load_result_t result = potop_map_load(potop, actions.map_name);

			SDL_Log(result == POTOP_LOAD_DONE ? "Loaded \"%s\"" : result == POTOP_LOAD_DAMAGED ? "\"%s\" is damaged: parts of it are missing"
				: "Could not load \"%s\"", actions.map_name);
			potop->map_loaded = result != POTOP_LOAD_FAILED;
		}
		if (actions.save_map)
		{
			SDL_Log(potop_map_save(potop, actions.map_name, actions.map_baked) ? "Saved \"%s\"" : "Could not save \"%s\"", actions.map_name);
		}
		if (actions.screenshot)
		{
			potop->screenshot_requested = true;
		}
		if (actions.quit)
		{
			potop->input.request_quit = true;
		}
	}
}

// The settings window (laid out with the HUD) and what it asks
static void settings_requests(potop_t *potop)
{
	settings_window_view_t view;
	const settings_window_requests_t *asked = &potop->settings_window.requests;

	SDL_zero(view);
	view.depth_scaled_pipes = potop->world.water.depth_scaled_pipes;
	view.seed = potop->world.terrain.seed;
	if (potop->ui_ready)
	{
		settings_window_draw(&potop->settings_window, &potop->backend, &potop->style, &potop->settings, &view);
		ui_backend_update_capture(&potop->backend);
	}
	if (asked->restore_settings)
	{
		potop->settings = potop->file_settings;
		world_apply_render_settings(&potop->world);
		hud_notify(&potop->hud, "Settings back to data/settings.ini", 3.0f, HUD_NOTIFY_INFO);
	}
	if (asked->apply_render)
	{
		world_apply_render_settings(&potop->world);
	}
	if (asked->apply_objects || asked->regrow)
	{
		world_regrow(&potop->world);
	}
	if (asked->remove_objects)
	{
		objects_clear(&potop->world.objects, false);
	}
	if (asked->save_settings)
	{
		hud_notify(&potop->hud, settings_save_overrides(&potop->settings, &potop->file_settings) ? "Settings saved" : "Could not save the settings", 3.0f,
			HUD_NOTIFY_INFO);
	}
	if (asked->new_map)
	{
		potop_map_new(potop, asked->new_map_seed);
	}
	if (asked->toggle_water_model)
	{
		world_touch_water(&potop->world);
		potop->world.water.depth_scaled_pipes = !potop->world.water.depth_scaled_pipes;
		water_wake_cells(&potop->world.water, 0, 0, potop->world.water.size - 1, potop->world.water.size - 1);
	}
	if (asked->bake_sky_occlusion)
	{
		potop->world.sky_occlusion.requested = true;
	}
}

// The weapon in hand, over everything of the world (world_render's hook)
static void draw_viewmodel(void *user, SDL_GPURenderPass *pass, int scene_width, int scene_height)
{
	potop_t *potop = user;
	const camera_t *camera = &potop->camera;
	float walking;

	if (!camera->first_person)
	{
		return;
	}
	walking = SDL_sqrtf(camera->walker_velocity_x * camera->walker_velocity_x + camera->walker_velocity_z * camera->walker_velocity_z);
	// (the torch in the other hand lights the weapon too, as much as the
	// night lets it)
	if (potop->session.player.light_on)
	{
		const light_source_t *light = &potop->session.player.light;
		float darkness = lights_darkness(potop->world.daylight.sun_elevation_degrees);
		float color[3];

		for (int k = 0; k < 3; k++)
		{
			color[k] = SDL_powf(light->color[k], 2.2f) * light->intensity * darkness * potop->world.lit_render.local_light_intensity;
		}
		viewmodel_set_carried_light(&potop->session.viewmodel, potop->session.player.light_position, color);
	}
	else
	{
		viewmodel_set_carried_light(&potop->session.viewmodel, NULL, NULL);
	}
	viewmodel_draw(&potop->session.viewmodel, pass, &potop->session.weapons, camera, &potop->world.lit_render,
		(float)scene_width / (float)SDL_max(scene_height, 1), camera->walker_on_ground ? walking : 0.0f, potop->session.player.weapon_seconds);
}

// The navigation mesh shown in the world (F4): the rectangles round the
// eye as shapes for the HUD, lying on what they are (the ground's follow
// the terrain, in pieces; an object's top at its height, its height over
// the ground written on it), their edges, and the portals between them
#define NAVMESH_OVERLAY_METERS 30.0f
#define NAVMESH_OVERLAY_PIECE_METERS 2.0f
#define NAVMESH_OVERLAY_LABEL_METERS 15.0f
#define NAVMESH_OVERLAY_SHAPES 8000
#define NAVMESH_OVERLAY_RECTS 2048
#define NAVMESH_OVERLAY_PORTALS 4096
#define NAVMESH_OVERLAY_CAPSULES 1024

typedef struct
{
	const camera_t *camera;
	const terrain_t *terrain;
	float width;
	float height;
	hud_world_shape_t *shapes;
	int count;
} navmesh_overlay_t;

// A point of the world on the screen (false: behind the eye)
static bool overlay_point(const navmesh_overlay_t *overlay, float x, float y, float z, float screen_out[2], float *depth_out)
{
	vec4_t point = { x, y, z, 1.0f };
	vec4_t clip = mat4_transform(overlay->camera->view_projection, point);

	if (clip.w <= 0.1f)
	{
		return false;
	}
	screen_out[0] = (clip.x / clip.w * 0.5f + 0.5f) * overlay->width;
	screen_out[1] = (0.5f - clip.y / clip.w * 0.5f) * overlay->height;
	*depth_out = clip.z / clip.w;
	return true;
}

// A point's height: the terrain's there (the ground's), or a given one
static float overlay_height(const navmesh_overlay_t *overlay, bool ground, float x, float z, float height, float lift)
{
	return (ground ? terrain_sample_height_meters(overlay->terrain, x, z) : height) + lift;
}

static bool overlay_near(const navmesh_overlay_t *overlay, float x, float z, float meters)
{
	float dx = x - overlay->camera->eye_position.x;
	float dz = z - overlay->camera->eye_position.z;

	return dx * dx + dz * dz <= meters * meters;
}

// A line from one point to another, in pieces (following the terrain)
static void overlay_line(navmesh_overlay_t *overlay, bool ground, float x0, float z0, float x1, float z1, float height, float lift,
	const uint8_t color[4], float thickness)
{
	float length = SDL_sqrtf((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
	int pieces = SDL_max((int)SDL_ceilf(length / NAVMESH_OVERLAY_PIECE_METERS), 1);

	for (int k = 0; k < pieces && overlay->count < NAVMESH_OVERLAY_SHAPES; k++)
	{
		float from = (float)k / (float)pieces;
		float to = (float)(k + 1) / (float)pieces;
		float ax = x0 + (x1 - x0) * from;
		float az = z0 + (z1 - z0) * from;
		float bx = x0 + (x1 - x0) * to;
		float bz = z0 + (z1 - z0) * to;
		hud_world_shape_t *shape = &overlay->shapes[overlay->count];
		float depth_a;
		float depth_b;

		if (!overlay_near(overlay, 0.5f * (ax + bx), 0.5f * (az + bz), NAVMESH_OVERLAY_METERS)
			|| !overlay_point(overlay, ax, overlay_height(overlay, ground, ax, az, height, lift), az, &shape->points[0], &depth_a)
			|| !overlay_point(overlay, bx, overlay_height(overlay, ground, bx, bz, height, lift), bz, &shape->points[2], &depth_b))
		{
			continue;
		}
		shape->count = 2;
		SDL_memcpy(shape->color, color, 4);
		shape->thickness = thickness;
		shape->depth = SDL_min(depth_a, depth_b);
		shape->label[0] = '\0';
		overlay->count++;
	}
}

static void fill_navmesh_overlay(potop_t *potop, hud_view_t *view)
{
	static hud_world_shape_t shapes[NAVMESH_OVERLAY_SHAPES];
	static navigation_debug_rect_t rects[NAVMESH_OVERLAY_RECTS];
	static navigation_debug_portal_t portals[NAVMESH_OVERLAY_PORTALS];
	// (the ground's rectangles told apart: a few tints, by their number)
	static const uint8_t ground_tints[4][3] = { { 60, 200, 120 }, { 60, 170, 225 }, { 130, 210, 70 }, { 40, 200, 195 } };
	static const uint8_t portal_color[4] = { 255, 255, 255, 235 };
	static const uint8_t jump_color[4] = { 255, 70, 215, 245 };
	static const uint8_t round_color[4] = { 255, 150, 30, 245 };
	static const uint8_t soft_color[4] = { 90, 160, 255, 245 };
	static navigation_capsule_t capsules[NAVMESH_OVERLAY_CAPSULES];
	int capsule_count;
	navmesh_overlay_t overlay;
	const camera_t *camera = &potop->camera;
	int rect_count = 0;
	int portal_count = 0;

	if (potop->navmesh_overlay_class < 0 || !potop->ui_ready)
	{
		return;
	}
	overlay.camera = camera;
	overlay.terrain = &potop->world.terrain;
	overlay.width = (float)potop->backend.width;
	overlay.height = (float)potop->backend.height;
	overlay.shapes = shapes;
	overlay.count = 0;
	navigation_mesh_debug(&potop->world.navigation, &potop->world.water, potop->navmesh_overlay_class,
		camera->eye_position.x - NAVMESH_OVERLAY_METERS, camera->eye_position.z - NAVMESH_OVERLAY_METERS,
		camera->eye_position.x + NAVMESH_OVERLAY_METERS, camera->eye_position.z + NAVMESH_OVERLAY_METERS, rects, NAVMESH_OVERLAY_RECTS, &rect_count,
		portals, NAVMESH_OVERLAY_PORTALS, &portal_count);
	for (int r = 0; r < rect_count; r++)
	{
		const navigation_debug_rect_t *rect = &rects[r];
		float middle_x = 0.5f * (rect->x0 + rect->x1);
		float middle_z = 0.5f * (rect->z0 + rect->z1);
		float over_ground = rect->height_high - terrain_sample_height_meters(overlay.terrain, middle_x, middle_z);
		uint8_t fill[4];
		uint8_t edge[4];
		int across = SDL_max((int)SDL_ceilf((rect->x1 - rect->x0) / NAVMESH_OVERLAY_PIECE_METERS), 1);
		int deep = SDL_max((int)SDL_ceilf((rect->z1 - rect->z0) / NAVMESH_OVERLAY_PIECE_METERS), 1);

		if (rect->ground)
		{
			SDL_memcpy(fill, ground_tints[(rect->id * 7 + (rect->id >> 16) * 3) & 3], 3);
			fill[3] = 60;
		}
		else
		{
			// (yellow at the ground, red a meter and a half up, purple from
			// three meters)
			float amount = SDL_clamp(over_ground / 3.0f, 0.0f, 1.0f);
			float first = SDL_clamp(amount * 2.0f, 0.0f, 1.0f);
			float second = SDL_clamp(amount * 2.0f - 1.0f, 0.0f, 1.0f);

			fill[0] = (uint8_t)(255.0f + (240.0f - 255.0f) * first + (170.0f - 240.0f) * second);
			fill[1] = (uint8_t)(225.0f + (70.0f - 225.0f) * first + (60.0f - 70.0f) * second);
			fill[2] = (uint8_t)(60.0f + (50.0f - 60.0f) * first + (225.0f - 50.0f) * second);
			fill[3] = 105;
		}
		SDL_memcpy(edge, fill, 3);
		edge[3] = 230;
		for (int j = 0; j < deep; j++)
		{
			for (int i = 0; i < across && overlay.count < NAVMESH_OVERLAY_SHAPES; i++)
			{
				float x0 = rect->x0 + (rect->x1 - rect->x0) * (float)i / (float)across;
				float x1 = rect->x0 + (rect->x1 - rect->x0) * (float)(i + 1) / (float)across;
				float z0 = rect->z0 + (rect->z1 - rect->z0) * (float)j / (float)deep;
				float z1 = rect->z0 + (rect->z1 - rect->z0) * (float)(j + 1) / (float)deep;
				const float corners[4][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
				hud_world_shape_t *shape = &overlay.shapes[overlay.count];
				float depth = 1.0f;
				bool seen = overlay_near(&overlay, 0.5f * (x0 + x1), 0.5f * (z0 + z1), NAVMESH_OVERLAY_METERS);

				for (int k = 0; k < 4 && seen; k++)
				{
					float corner_depth;

					seen = overlay_point(&overlay, corners[k][0], overlay_height(&overlay, rect->ground, corners[k][0], corners[k][1],
						rect->height_high, 0.05f), corners[k][1], &shape->points[2 * k], &corner_depth);
					depth = SDL_min(depth, corner_depth);
				}
				if (!seen)
				{
					continue;
				}
				shape->count = 4;
				SDL_memcpy(shape->color, fill, 4);
				shape->thickness = 0.0f;
				shape->depth = depth;
				shape->label[0] = '\0';
				overlay.count++;
			}
		}
		overlay_line(&overlay, rect->ground, rect->x0, rect->z0, rect->x1, rect->z0, rect->height_high, 0.07f, edge, 1.5f);
		overlay_line(&overlay, rect->ground, rect->x1, rect->z0, rect->x1, rect->z1, rect->height_high, 0.07f, edge, 1.5f);
		overlay_line(&overlay, rect->ground, rect->x1, rect->z1, rect->x0, rect->z1, rect->height_high, 0.07f, edge, 1.5f);
		overlay_line(&overlay, rect->ground, rect->x0, rect->z1, rect->x0, rect->z0, rect->height_high, 0.07f, edge, 1.5f);
		// (an object's top: how high over the ground, written on it)
		if (!rect->ground && overlay.count < NAVMESH_OVERLAY_SHAPES && overlay_near(&overlay, middle_x, middle_z, NAVMESH_OVERLAY_LABEL_METERS))
		{
			hud_world_shape_t *shape = &overlay.shapes[overlay.count];

			if (overlay_point(&overlay, middle_x, rect->height_high + 0.1f, middle_z, &shape->points[0], &shape->depth))
			{
				shape->count = 0;
				SDL_snprintf(shape->label, sizeof(shape->label), "%.2f", (double)over_ground);
				overlay.count++;
			}
		}
	}
	for (int p = 0; p < portal_count; p++)
	{
		const navigation_debug_portal_t *portal = &portals[p];

		overlay_line(&overlay, portal->ground, portal->a[0], portal->a[1], portal->b[0], portal->b[1], portal->height, 0.12f,
			portal->jump ? jump_color : portal_color, portal->jump ? 3.5f : 2.5f);
	}
	// The small things' capsules (what a way is led round: orange; soft
	// ones, run through: blue): a ring at each end, as wide as it
	capsule_count = navigation_capsules(&potop->world.navigation, camera->eye_position.x - NAVMESH_OVERLAY_METERS,
		camera->eye_position.z - NAVMESH_OVERLAY_METERS, camera->eye_position.x + NAVMESH_OVERLAY_METERS,
		camera->eye_position.z + NAVMESH_OVERLAY_METERS, capsules, NAVMESH_OVERLAY_CAPSULES);
	for (int k = 0; k < capsule_count; k++)
	{
		const navigation_capsule_t *capsule = &capsules[k];
		const uint8_t *color = capsule->soft != 0 ? soft_color : round_color;

		for (int end = 0; end < 2; end++)
		{
			const float *point = end == 0 ? capsule->a : capsule->b;

			for (int side = 0; side < 8; side++)
			{
				float from = (float)side * SDL_PI_F / 4.0f;
				float to = (float)(side + 1) * SDL_PI_F / 4.0f;

				overlay_line(&overlay, false, point[0] + SDL_cosf(from) * capsule->radius, point[2] + SDL_sinf(from) * capsule->radius,
					point[0] + SDL_cosf(to) * capsule->radius, point[2] + SDL_sinf(to) * capsule->radius, point[1], end == 0 ? 0.1f : 0.0f,
					color, 2.0f);
			}
		}
	}
	view->world_shapes = shapes;
	view->world_shape_count = overlay.count;
}

void potop_frame_end(potop_t *potop)
{
	float seconds = potop->frame_seconds;
	camera_t *camera = &potop->camera;
	int pixel_width;
	int pixel_height;
	hud_view_t view;
	uint64_t section_start = SDL_GetPerformanceCounter();

	frame_profile_end(FRAME_GAME, potop->game_counter);
	// (--bake_paths: once the world has been through a frame or two, the
	// navigation knowing its map)
	if (potop->bake_paths_frames > 0 && --potop->bake_paths_frames == 0)
	{
		potop_bake_paths(potop);
	}
	// (--take: a test's object gone, its time come; how many tiles of the
	// navigation were built till then is told, to compare after)
	while (potop->take_done < potop->take_count && potop->sim_elapsed >= (double)potop->take_seconds[potop->take_done])
	{
		const float *point = potop->take_points[potop->take_done++];
		bool taken = objects_remove_nearest(&potop->world.objects, point[0], point[1], 1.0f);

		SDL_Log("Take: the object at (%.2f, %.2f) %s, %d navigation tiles built so far", (double)point[0], (double)point[1],
			taken ? "taken away" : "NOT FOUND", (int)potop->world.navigation.mesh.tiles_built);
		potop->take_told = false;
	}
	// (--push: the object nearest the point pushed, its time come; a while
	// after, where it lies and the navigation's capsule nearest it)
	if (potop->push_stage == 1 && potop->sim_elapsed >= (double)potop->push_seconds)
	{
		objects_t *objects = &potop->world.objects;
		int nearest = -1;
		float nearest_squared = 1.0f;

		for (int k = 0; k < objects->instance_count; k++)
		{
			float dx = objects->instances[k].x - potop->push_values[0];
			float dz = objects->instances[k].z - potop->push_values[1];

			if ((objects->instances[k].state & OBJECT_STATE_DEAD) == 0 && dx * dx + dz * dz < nearest_squared)
			{
				nearest = k;
				nearest_squared = dx * dx + dz * dz;
			}
		}
		potop->push_stage = 0;
		if (nearest >= 0 && objects->instances[nearest].body >= 0)
		{
			float impulse[3] = { potop->push_values[2], 0.0f, potop->push_values[3] };
			float point[3] = { objects->instances[nearest].x, objects_instance_base_y(&objects->instances[nearest]) + 0.3f,
				objects->instances[nearest].z };

			physics_push_body(&potop->world.physics, objects->instances[nearest].body, impulse, point);
			potop->push_id = objects->instances[nearest].id;
			potop->push_stage = 2;
		}
		SDL_Log("Push: the object at (%.2f, %.2f) %s", (double)potop->push_values[0], (double)potop->push_values[1],
			potop->push_stage == 2 ? "pushed" : "NOT FOUND (or it has no body)");
	}
	if (potop->push_stage == 2 && potop->sim_elapsed >= (double)potop->push_seconds + POTOP_TAKE_AFTER_SECONDS)
	{
		const objects_t *objects = &potop->world.objects;

		potop->push_stage = 0;
		for (int k = 0; k < objects->instance_count; k++)
		{
			const object_instance_t *instance = &objects->instances[k];
			navigation_capsule_t capsules[64];
			int count;
			float best = 1e30f;
			float best_x = 0.0f;
			float best_z = 0.0f;

			if (instance->id != potop->push_id)
			{
				continue;
			}
			count = navigation_capsules(&potop->world.navigation, instance->x - 8.0f, instance->z - 8.0f, instance->x + 8.0f, instance->z + 8.0f,
				capsules, (int)SDL_arraysize(capsules));
			for (int n = 0; n < count; n++)
			{
				float dx = 0.5f * (capsules[n].a[0] + capsules[n].b[0]) - instance->x;
				float dz = 0.5f * (capsules[n].a[2] + capsules[n].b[2]) - instance->z;

				if (capsules[n].id == instance->id && dx * dx + dz * dz < best)
				{
					best = dx * dx + dz * dz;
					best_x = instance->x + dx;
					best_z = instance->z + dz;
				}
			}
			SDL_Log("Push: %.0f s after it lies at (%.2f, %.2f), %s; its capsule in the navigation %s (%.2f, %.2f)", POTOP_TAKE_AFTER_SECONDS,
				(double)instance->x, (double)instance->z, (instance->state & OBJECT_STATE_AWAKE) != 0 ? "moving" : "at rest",
				best < 1e29f ? "at" : "NONE", (double)best_x, (double)best_z);
			break;
		}
	}
	// (and how many a while after the last: what its going built again)
	if (potop->take_count > 0 && potop->take_done == potop->take_count && !potop->take_told
		&& potop->sim_elapsed >= (double)potop->take_seconds[potop->take_count - 1] + POTOP_TAKE_AFTER_SECONDS)
	{
		potop->take_told = true;
		SDL_Log("Take: %d navigation tiles built %.0f s after", (int)potop->world.navigation.mesh.tiles_built, POTOP_TAKE_AFTER_SECONDS);
	}
	// The HUD laid out (what the game said this frame), what it asks
	fill_hud_view(potop, &view);
	fill_navmesh_overlay(potop, &view);
	hud_update(&potop->hud, potop->ui_ready ? &potop->backend : NULL, potop->style.colors, &view, potop->world.elapsed_seconds);
	if (potop->ui_ready)
	{
		ui_backend_update_capture(&potop->backend);
	}
	handle_hud_requests(potop);
	settings_requests(potop);
	run_console(potop);
	world_follow_settings(&potop->world);
	gpu_set_vsync(potop->settings.render.vsync);
	gpu_set_anisotropy(potop->settings.render.anisotropy);
	session_update(&potop->session, &potop->world, seconds);
	journal_sync_markers(&potop->session.journal, &potop->session.markers);
	// The map opened: its picture made again if stale
	if (potop->hud.map_open && !potop->session.map_was_open)
	{
		char shown[MAP_FILE_NAME_LENGTH];

		map_name_shown(potop->world.map_name, shown, sizeof(shown));
		session_update_world_map(&potop->session, &potop->world, shown);
	}
	potop->session.map_was_open = potop->hud.map_open;
	frame_profile_end(FRAME_HUD, section_start);
	section_start = SDL_GetPerformanceCounter();
	// The walker, the eye
	SDL_GetWindowSizeInPixels(potop->window, &pixel_width, &pixel_height);
	pixel_width = SDL_max(pixel_width, 1);
	pixel_height = SDL_max(pixel_height, 1);
	if (potop->player_held)
	{
		potop->input.pan_keyboard_x = 0.0f;
		potop->input.pan_keyboard_y = 0.0f;
		potop->input.jump_pressed = false;
		potop->input.jump_held = false;
	}
	if (camera->first_person)
	{
		player_walk(&potop->session.player, &potop->world, camera, &potop->input, &potop->settings.camera, seconds);
	}
	else
	{
		camera_update(camera, &potop->input, &potop->settings.camera, &potop->world.terrain, seconds, pixel_height);
	}
	camera_compute_matrices(camera, &potop->settings.camera, &potop->world.terrain, (float)pixel_width / (float)pixel_height);
	update_look_pick(potop, pixel_width, pixel_height);
	potop_update_steps(potop, seconds);
	player_update_light(&potop->session.player, &potop->world, camera);
	frame_profile_end(FRAME_PLAYER, section_start);
	// The world: the actors, the fixed steps, the weapon and its shots
	section_start = SDL_GetPerformanceCounter();
	world_update_actors(&potop->world, seconds);
	frame_profile_end(FRAME_ACTORS, section_start);
	world_simulate(&potop->world, seconds, camera->first_person ? camera->walker_x : camera->target_x,
		camera->first_person ? camera->walker_z : camera->target_z, false, NULL, NULL);
	section_start = SDL_GetPerformanceCounter();
	player_update_weapons(&potop->session.player, &potop->session, &potop->world, camera, seconds);
	{
		// (the player's middle, for the proximity fuses and the blasts)
		float player[3] = { camera->eye_position.x, camera->eye_position.y - 0.7f, camera->eye_position.z };

		explosives_update(&potop->session.explosives, &potop->session, &potop->world, player, seconds);
		potop_update_areas(potop);
	}
	frame_profile_end(FRAME_WEAPONS, section_start);
	world_end_frame(&potop->world, camera, seconds, false, false);
	trace_mark("the game, the HUD, the player, the actors, the weapons");
	// A test without the picture: what was recorded (the water's steps,
	// uploads) sent, nothing drawn
	if (potop->sim_seconds > 0.0f)
	{
		// (the navigation hears of the objects that changed, as it does
		// each frame the picture is drawn: world_render)
		navigation_update(&potop->world.navigation, &potop->world.terrain, &potop->world.objects, potop->world.elapsed_seconds);
		gpu_flush_uploads();
		gpu_submit_work(NULL);
		trace_end();
		frame_profile_finish();
		potop->game_counter = SDL_GetPerformanceCounter();
		return;
	}
	// The picture
	gpu_begin_frame();
	trace_mark("the swapchain acquired (waits for the GPU)");
	{
		world_render_options_t options;

		SDL_zero(options);
		options.draw_in_scene = draw_viewmodel;
		options.user = potop;
		section_start = SDL_GetPerformanceCounter();
		world_render(&potop->world, camera, &options, pixel_width, pixel_height);
		frame_profile_end(FRAME_RENDER, section_start);
	}
	section_start = SDL_GetPerformanceCounter();
	if (potop->ui_ready)
	{
		// (the names over the actors hide behind what stands nearer)
		ui_backend_set_scene_depth(&potop->backend, potop->world.scene_target.depth_texture);
		ui_backend_render(&potop->backend, gpu.final_texture != NULL ? (int)gpu.final_width : pixel_width,
			gpu.final_texture != NULL ? (int)gpu.final_height : pixel_height);
	}
	frame_profile_end(FRAME_UI, section_start);
	trace_mark("the HUD drawn");
	{
		uint8_t *pixels = NULL;
		uint32_t shot_width = 0;
		uint32_t shot_height = 0;

		gpu_end_frame(potop->screenshot_requested ? &pixels : NULL, &shot_width, &shot_height);
		if (pixels != NULL)
		{
			char name[128];

			screenshot_make_name(name, (int)sizeof(name));
			screenshot_save(name, pixels, (int)shot_width, (int)shot_height);
			SDL_free(pixels);
		}
		potop->screenshot_requested = false;
	}
	trace_mark("the frame submitted, acquired, presented");
	// (the sky's occlusion, just worked out: on the CPU too, for the light)
	section_start = SDL_GetPerformanceCounter();
	sky_occlusion_between_frames(&potop->world.sky_occlusion);
	frame_profile_end(FRAME_SKY_OCCLUSION, section_start);
	// The interface's size or font changed (the settings' Apply): the HUD's
	// backend made again
	if (potop->ui_ready && (potop->style.scale != potop->backend.scale || potop->style.font_size != potop->backend.font->handle.height
		|| potop->style.dialog_text_scale != potop->backend.large_scale))
	{
		ui_backend_destroy(&potop->backend);
		potop->ui_ready = ui_backend_create(&potop->backend, potop->window, potop->style.font_path, potop->style.font_size,
			potop->style.dialog_text_scale, potop->style.scale);
		if (potop->ui_ready)
		{
			ui_style_apply(&potop->style, &potop->backend.context);
		}
	}
	trace_end();
	frame_profile_finish();
	section_start = SDL_GetPerformanceCounter();
	// The frame rate limit: the rest of the frame's time waited out (each
	// frame a period after the one before; fallen a whole period behind, the
	// count starts over from now)
	if (potop->settings.render.max_fps > 0.0f && !potop->input.in_background)
	{
		uint64_t period = (uint64_t)(1e9 / (double)SDL_max(potop->settings.render.max_fps, 1.0f));
		uint64_t now = SDL_GetTicksNS();
		uint64_t next = potop->next_frame_ns;

		if (next == 0 || now >= next + period)
		{
			next = now;
		}
		else if (now < next)
		{
			SDL_DelayPrecise(next - now);
		}
		potop->next_frame_ns = next + period;
	}
	else
	{
		potop->next_frame_ns = 0;
	}
	// In the background vsync often does not hold the loop back: a rest
	if (potop->input.in_background)
	{
		SDL_Delay(30);
	}
	// (both counted in the frame they begin, under their own name: the
	// rest was the game's 30 ms, a wait overslept in no section at all)
	frame_profile_end(FRAME_LIMIT, section_start);
	potop->game_counter = SDL_GetPerformanceCounter();
}

void potop_player_reload(potop_t *potop)
{
	weapons_reload(&potop->session.weapons);
}

void potop_player_set_held(potop_t *potop, bool held)
{
	potop->player_held = held;
}

bool potop_console_add_command(potop_t *potop, const char *name, const char *usage, const char *help,
	void (*function)(void *user, const char *arguments), void *user)
{
	return console_add_command(&potop->console, name, usage, help, function, user);
}

void potop_settings_open(potop_t *potop, bool open)
{
	potop->settings_window.open = open;
}

bool potop_settings_is_open(const potop_t *potop)
{
	return potop->settings_window.open;
}

static light_source_t light_source_of(const potop_light_t *light)
{
	light_source_t source;

	light_source_reset(&source);
	SDL_memcpy(source.color, light->color, sizeof(source.color));
	source.intensity = light->intensity;
	source.radius_meters = light->radius_meters;
	source.flicker = light->flicker;
	source.core_meters = light->core_meters;
	source.switched = light->switched;
	return source;
}

void potop_player_set_light(potop_t *potop, const potop_light_t *light)
{
	player_t *player = &potop->session.player;

	player->light_on = light != NULL;
	if (light != NULL)
	{
		player->light = light_source_of(light);
	}
}

void potop_actor_set_light(potop_t *potop, uint32_t id, const potop_light_t *light)
{
	potop_actor_info_t actor;

	if (light == NULL)
	{
		lights_remove_carried(&potop->world.lights, PLAYER_ACTOR_LIGHT_KEYS + id);
	}
	else if (potop_actor_get(potop, id, &actor))
	{
		light_source_t source = light_source_of(light);
		float position[3] = { actor.x, actor.top_y, actor.z };

		// (where it is held: world_end_frame follows the actor)
		lights_set_carried(&potop->world.lights, PLAYER_ACTOR_LIGHT_KEYS + id, position, &source);
	}
}

void potop_flash(potop_t *potop, float x, float y, float z, const potop_light_t *light, float seconds)
{
	light_source_t source = light_source_of(light);
	float position[3] = { x, y, z };

	lights_flash(&potop->world.lights, position, &source, seconds, potop->world.elapsed_seconds);
}
