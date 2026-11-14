// house_editor: potop_house in a window. The house in view (drawn with the
// game's building shaders, tools/house_preview.c), made again a moment
// after each change (or on Apply, F5). A desktop tool: not part of the
// game, never shipped with it.
//
//   house_editor [house_plans/name.house] [--data DIR]
//
// On the left, the recipe (see tools/house_grow.h): a preset to start from,
// its kind, walls and roof, the seed, the numbers that shape it, the
// textures. Save writes the recipe (house_plans/) and the object (.pobj,
// under data/objects/) with its variants (the next seeds, name_1.pobj...):
// the game, if it runs (potop --houses), shows the new house within a
// second.
//
// While the numbers change the house is made without its weathering baked
// (every vertex open to the sky: a moment's work); once they rest, the
// baking (how much sky each vertex sees, how sheltered it is: the
// seconds a big house takes) goes on in the background and the house is
// swapped for the baked one when it is done. Save always bakes.
//
// On the right, the house: dragging with the left button (or the wheel's)
// turns round it, with the right one moves the eye up and down, the wheel
// comes nearer. F12 (or Print Screen): a screenshot.
//
// For the tests: --screenshot FILE [--frames N] draws N frames (30), writes
// what was on screen and leaves; --save saves first; --preset NAME starts
// from a preset; --yaw, --pitch, --distance (degrees, meters) the view.

// (realpath: POSIX)
#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "house_grow.h"
#include "house_preview.h"
#include "post.h"
#include "render_target.h"
#include "screenshot.h"
#include "season.h"
#include "settings.h"
#include "ui_backend.h"

#define PANEL_WIDTH 440.0f
#define FONT_SIZE 15.0f
#define ROW 24.0f
// A change waits this long (seconds) for the next before the house is made
// again (dragging a slider: once it rests), and the baking this long more
#define GROW_DELAY 0.12
#define BAKE_DELAY 0.6
#define TEXTURED_MATERIALS 5             // walls, timber, roof, base, glass: the ones with textures of their own

typedef enum
{
	PICK_NONE,
	PICK_OPEN_RECIPE,
	PICK_SAVE_RECIPE,
	PICK_TEXTURE                       // which: pick_material, pick_map
} pick_t;

// The baking in the background: one at a time; its house is taken if the
// recipe is still the one it was baked for
typedef enum
{
	BAKE_IDLE,
	BAKE_RUNNING,
	BAKE_DONE,
	BAKE_FAILED
} bake_state_t;

typedef struct
{
	SDL_Thread *thread;
	SDL_AtomicInt state;               // bake_state_t
	house_recipe_t recipe;
	const char *data_directory;
	house_mesh_t mesh;
	double seconds;
} bake_t;

typedef struct
{
	SDL_Window *window;
	ui_backend_t ui;
	settings_t settings;
	render_target_t target;
	post_t post;
	house_preview_t preview;
	const char *data_directory;

	house_recipe_t recipe;
	house_recipe_t grown_recipe;       // the recipe of the house in view
	house_recipe_t last_seen;          // the recipe the frame before (a change's time)
	double changed_at;
	bool apply_asked;
	bool auto_apply;
	bool grown_once;
	bool baked;                        // the house in view has its weathering baked
	bool bake_wanted;                  // bake in the background once the numbers rest
	char recipe_path[HOUSE_PATH_LENGTH];
	house_mesh_t mesh;
	bool mesh_ok;
	double grow_seconds;
	double bake_seconds;
	bake_t bake;
	char message[1024];
	bool message_is_error;
	int preset_index;
	int variants;

	float yaw_degrees;
	float pitch_degrees;
	float distance;
	float look_height;
	float sun_azimuth_degrees;
	float sun_elevation_degrees;
	float ambient;
	float month;
	float wetness;
	float age;
	float dirt_meters;
	int detail;
	int show;
	int hide;                          // a material not drawn: 0 none, else its index + 1
	bool show_collision;
	bool turning;
	bool raising;

	SDL_Mutex *pick_lock;
	pick_t pick_asked;
	int pick_material;
	int pick_map;
	bool pick_answered;
	char picked[HOUSE_PATH_LENGTH];
	int picked_count;
	bool save_pending;
} editor_t;

static void save(editor_t *editor);

// ---------------------------------------------------------------------------
// Paths

static void relative_path(const char *path, char *out, size_t out_size)
{
	char *here = SDL_GetCurrentDirectory();
	size_t length = here != NULL ? strlen(here) : 0;

	if (here != NULL && length > 0 && strncmp(path, here, length) == 0)
	{
		snprintf(out, out_size, "%s", path + length);
	}
	else
	{
		snprintf(out, out_size, "%s", path);
	}
	SDL_free(here);
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

static bool ends_with(const char *text, const char *end)
{
	size_t length = strlen(text);
	size_t end_length = strlen(end);

	return length >= end_length && strcmp(text + length - end_length, end) == 0;
}

static void text_field(editor_t *editor, struct nk_context *context, char *text, int size)
{
	nk_flags state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, text, size, nk_filter_default);

	if (state & NK_EDIT_ACTIVE)
	{
		editor->ui.wants_text_input = true;
	}
}

// A picked texture as the game names it (relative to data/)
static bool texture_name(const editor_t *editor, const char *path, char *out, size_t out_size)
{
	char real_data[4096];
	char real_path[4096];
	size_t length;

	if (realpath(editor->data_directory, real_data) == NULL || realpath(path, real_path) == NULL)
	{
		return false;
	}
	length = strlen(real_data);
	if (strncmp(real_path, real_data, length) != 0 || real_path[length] != '/')
	{
		return false;
	}
	snprintf(out, out_size, "%s", real_path + length + 1);
	return true;
}

// The object named as the recipe (objects/<name>.pobj, under the data
// directory, as potop_house --all names it)
static void output_named_as_recipe(editor_t *editor)
{
	char name[HOUSE_PATH_LENGTH];
	char *dot;

	snprintf(name, sizeof(name), "%s", base_name(editor->recipe_path));
	dot = strrchr(name, '.');
	if (dot != NULL)
	{
		*dot = '\0';
	}
	if (name[0] != '\0')
	{
		snprintf(editor->recipe.output, sizeof(editor->recipe.output), "objects/%s.pobj", name);
	}
}

// ---------------------------------------------------------------------------
// File dialogs

static void SDLCALL dialog_answered(void *user_data, const char *const *files, int filter)
{
	editor_t *editor = user_data;

	(void)filter;
	SDL_LockMutex(editor->pick_lock);
	editor->picked_count = 0;
	if (files != NULL && files[0] != NULL)
	{
		relative_path(files[0], editor->picked, HOUSE_PATH_LENGTH);
		editor->picked_count = 1;
	}
	if (files == NULL)
	{
		editor->picked_count = -1;
		snprintf(editor->picked, HOUSE_PATH_LENGTH, "%s", SDL_GetError());
	}
	editor->pick_answered = true;
	SDL_UnlockMutex(editor->pick_lock);
}

static void ask_file(editor_t *editor, pick_t pick, int material, int map, bool save_dialog)
{
	static const SDL_DialogFileFilter pictures[] = { { "Pictures", "png;jpg;jpeg" } };
	static const SDL_DialogFileFilter recipes[] = { { "House recipes", "house" } };
	const SDL_DialogFileFilter *filters = pick == PICK_TEXTURE ? pictures : recipes;
	char *here = SDL_GetCurrentDirectory();

	if (editor->pick_asked != PICK_NONE)
	{
		SDL_free(here);
		return;
	}
	editor->pick_asked = pick;
	editor->pick_material = material;
	editor->pick_map = map;
	if (save_dialog)
	{
		SDL_ShowSaveFileDialog(dialog_answered, editor, editor->window, filters, 1, here);
	}
	else
	{
		SDL_ShowOpenFileDialog(dialog_answered, editor, editor->window, filters, 1, here, false);
	}
	SDL_free(here);
}

static void say(editor_t *editor, bool error, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(editor->message, sizeof(editor->message), format, arguments);
	va_end(arguments);
	editor->message_is_error = error;
	SDL_Log("%s", editor->message);
}

static void open_recipe(editor_t *editor, const char *path)
{
	house_recipe_t loaded;

	house_recipe_defaults(&loaded);
	if (!house_recipe_load(&loaded, path))
	{
		say(editor, true, "%s", house_last_error());
		return;
	}
	editor->recipe = loaded;
	editor->apply_asked = true;
	snprintf(editor->recipe_path, sizeof(editor->recipe_path), "%s", path);
	if (editor->recipe.output[0] == '\0')
	{
		output_named_as_recipe(editor);
	}
	editor->variants = editor->recipe.variants;
	say(editor, false, "Opened %s", path);
}

static void take_answer(editor_t *editor)
{
	char picked[HOUSE_PATH_LENGTH];
	int count;
	pick_t pick;
	bool answered;

	SDL_LockMutex(editor->pick_lock);
	answered = editor->pick_answered;
	count = editor->picked_count;
	memcpy(picked, editor->picked, sizeof(picked));
	editor->pick_answered = false;
	SDL_UnlockMutex(editor->pick_lock);
	if (!answered)
	{
		return;
	}
	pick = editor->pick_asked;
	editor->pick_asked = PICK_NONE;
	if (count < 0)
	{
		say(editor, true, "No file dialog (%s): zenity, or the desktop's portal, is needed for one; type the paths instead", picked);
	}
	if (count <= 0)
	{
		editor->save_pending = false;
		return;
	}
	switch (pick)
	{
		case PICK_OPEN_RECIPE:
			open_recipe(editor, picked);
			break;
		case PICK_SAVE_RECIPE:
			snprintf(editor->recipe_path, sizeof(editor->recipe_path), "%s", picked);
			if (!ends_with(editor->recipe_path, ".house"))
			{
				SDL_strlcat(editor->recipe_path, ".house", sizeof(editor->recipe_path));
			}
			output_named_as_recipe(editor);
			if (editor->save_pending)
			{
				editor->save_pending = false;
				save(editor);
			}
			break;
		case PICK_TEXTURE:
		{
			char name[HOUSE_PATH_LENGTH];

			if (!texture_name(editor, picked, name, sizeof(name)))
			{
				say(editor, true, "'%s' is not under %s/: the game finds textures there only (copy it there first)", picked,
					editor->data_directory);
				break;
			}
			snprintf(editor->recipe.textures[editor->pick_material][editor->pick_map], HOUSE_PATH_LENGTH, "%s", name);
			break;
		}
		case PICK_NONE:
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Making it

static int SDLCALL bake_thread(void *data)
{
	bake_t *bake = data;
	uint64_t start = SDL_GetPerformanceCounter();
	bool ok = house_grow(&bake->recipe, bake->data_directory, &bake->mesh);

	bake->seconds = (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
	SDL_SetAtomicInt(&bake->state, ok ? BAKE_DONE : BAKE_FAILED);
	return 0;
}

// A finished baking taken (if still wanted) and its thread let go
static void take_bake(editor_t *editor)
{
	int state = SDL_GetAtomicInt(&editor->bake.state);

	if (state != BAKE_DONE && state != BAKE_FAILED)
	{
		return;
	}
	SDL_WaitThread(editor->bake.thread, NULL);
	editor->bake.thread = NULL;
	if (state == BAKE_DONE && memcmp(&editor->bake.recipe, &editor->grown_recipe, sizeof(house_recipe_t)) == 0)
	{
		house_mesh_free(&editor->mesh);
		editor->mesh = editor->bake.mesh;
		editor->mesh_ok = house_preview_set_mesh(&editor->preview, &editor->mesh, editor->data_directory);
		editor->baked = true;
		editor->bake_seconds = editor->bake.seconds;
	}
	else if (state == BAKE_DONE)
	{
		house_mesh_free(&editor->bake.mesh);
	}
	SDL_SetAtomicInt(&editor->bake.state, BAKE_IDLE);
}

static void start_bake(editor_t *editor)
{
	if (SDL_GetAtomicInt(&editor->bake.state) != BAKE_IDLE)
	{
		return;
	}
	editor->bake.recipe = editor->grown_recipe;
	editor->bake.data_directory = editor->data_directory;
	SDL_SetAtomicInt(&editor->bake.state, BAKE_RUNNING);
	editor->bake.thread = SDL_CreateThread(bake_thread, "house bake", &editor->bake);
	if (editor->bake.thread == NULL)
	{
		SDL_SetAtomicInt(&editor->bake.state, BAKE_IDLE);
	}
}

static void grow_if_changed(editor_t *editor, bool now)
{
	double seconds = (double)SDL_GetTicks() / 1000.0;

	take_bake(editor);
	if (memcmp(&editor->recipe, &editor->last_seen, sizeof(house_recipe_t)) != 0)
	{
		editor->last_seen = editor->recipe;
		editor->changed_at = seconds;
	}
	if (editor->grown_once && memcmp(&editor->recipe, &editor->grown_recipe, sizeof(house_recipe_t)) == 0)
	{
		// The same house: baked, once the numbers have rested a while
		if (editor->bake_wanted && editor->mesh_ok && !editor->baked && seconds - editor->changed_at >= BAKE_DELAY)
		{
			start_bake(editor);
		}
		return;
	}
	if (!now && editor->grown_once && !editor->apply_asked && (!editor->auto_apply || seconds - editor->changed_at < GROW_DELAY))
	{
		return;
	}
	editor->apply_asked = false;
	{
		house_mesh_t mesh;
		uint64_t start = SDL_GetPerformanceCounter();

		editor->grown_recipe = editor->recipe;
		editor->grown_once = true;
		if (!house_grow_quick(&editor->recipe, editor->data_directory, &mesh))
		{
			say(editor, true, "%s", house_last_error());
			return;
		}
		editor->grow_seconds = (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
		house_mesh_free(&editor->mesh);
		editor->mesh = mesh;
		editor->mesh_ok = house_preview_set_mesh(&editor->preview, &editor->mesh, editor->data_directory);
		editor->baked = false;
		if (editor->message_is_error)
		{
			editor->message[0] = '\0';
		}
	}
}

// The output's name with _N before the extension (as potop_house names
// the variants)
static void variant_name(const char *output, int number, char *out, size_t size)
{
	const char *dot = strrchr(output, '.');
	const char *slash = strrchr(output, '/');

	if (dot == NULL || (slash != NULL && dot < slash))
	{
		snprintf(out, size, "%s_%d", output, number);
	}
	else
	{
		snprintf(out, size, "%.*s_%d%s", (int)(dot - output), output, number, dot);
	}
}

static bool grow_and_write(editor_t *editor, const house_recipe_t *recipe, const char *output)
{
	house_mesh_t mesh;
	char path[HOUSE_PATH_LENGTH * 2];
	bool ok;

	if (!house_grow(recipe, editor->data_directory, &mesh))
	{
		say(editor, true, "%s", house_last_error());
		return false;
	}
	if (output[0] == '/')
	{
		snprintf(path, sizeof(path), "%s", output);
	}
	else
	{
		snprintf(path, sizeof(path), "%s/%s", editor->data_directory, output);
	}
	ok = house_write(path, &mesh);
	if (!ok)
	{
		say(editor, true, "%s", house_last_error());
	}
	house_mesh_free(&mesh);
	return ok;
}

static void save(editor_t *editor)
{
	if (editor->recipe_path[0] == '\0')
	{
		editor->save_pending = true;
		ask_file(editor, PICK_SAVE_RECIPE, 0, 0, true);
		return;
	}
	if (!ends_with(editor->recipe_path, ".house"))
	{
		SDL_strlcat(editor->recipe_path, ".house", sizeof(editor->recipe_path));
	}
	if (editor->recipe.output[0] == '\0')
	{
		output_named_as_recipe(editor);
	}
	editor->recipe.variants = editor->variants;
	if (!house_recipe_save(&editor->recipe, editor->recipe_path))
	{
		say(editor, true, "%s", house_last_error());
		return;
	}
	if (!grow_and_write(editor, &editor->recipe, editor->recipe.output))
	{
		return;
	}
	for (int number = 1; number <= editor->recipe.variants; number++)
	{
		house_recipe_t variant = editor->recipe;
		char name[HOUSE_PATH_LENGTH + 16];

		variant.seed = editor->recipe.seed + (uint32_t)number;
		variant_name(editor->recipe.output, number, name, sizeof(name));
		if (!grow_and_write(editor, &variant, name))
		{
			return;
		}
	}
	// (the variants' count is the recipe's: no change to make again for it)
	editor->last_seen.variants = editor->recipe.variants;
	editor->grown_recipe.variants = editor->recipe.variants;
	say(editor, false, "Saved %s and %s/%s%s", editor->recipe_path, editor->data_directory, editor->recipe.output,
		editor->recipe.variants > 0 ? " with its variants" : "");
}

// ---------------------------------------------------------------------------
// The panel

static void label(struct nk_context *context, const char *text)
{
	nk_label(context, text, NK_TEXT_LEFT);
}

static void slider(struct nk_context *context, const char *name, float *value, float minimum, float maximum, float step)
{
	nk_property_float(context, name, minimum, value, maximum, step, step * 0.5f);
}

// The same, shown times scale (centimeters: small numbers would show as
// 0.00, and a click's step would not show)
static void slider_scaled(struct nk_context *context, const char *name, float *value, float minimum, float maximum, float step, float scale)
{
	float shown = *value * scale;
	float before = shown;

	nk_property_float(context, name, minimum * scale, &shown, maximum * scale, step * scale, step * scale * 0.5f);
	if (shown != before)
	{
		*value = shown / scale;
	}
}

static void whole(struct nk_context *context, const char *name, int *value, int minimum, int maximum)
{
	nk_property_int(context, name, minimum, value, maximum, 1, 0.2f);
}

static void check(struct nk_context *context, const char *name, int *value)
{
	nk_bool on = *value != 0 ? nk_true : nk_false;

	nk_checkbox_label(context, name, &on);
	*value = on != nk_false ? 1 : 0;
}

// A labeled list of names: the value an index into it
static int choose(struct nk_context *context, const char *name, const char *const *names, int count, int value)
{
	nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
	nk_layout_row_push(context, 0.4f);
	label(context, name);
	nk_layout_row_push(context, 0.6f);
	value = nk_combo(context, names, count, SDL_clamp(value, 0, count - 1), (int)ROW, nk_vec2(240.0f, 300.0f));
	nk_layout_row_end(context);
	return value;
}

static void recipe_section(editor_t *editor)
{
	struct nk_context *context = &editor->ui.context;
	house_recipe_t *recipe = &editor->recipe;

	nk_layout_row_dynamic(context, ROW, 4);
	if (nk_button_label(context, "New"))
	{
		house_recipe_defaults(recipe);
		editor->recipe_path[0] = '\0';
		editor->apply_asked = true;
	}
	if (nk_button_label(context, "Open..."))
	{
		ask_file(editor, PICK_OPEN_RECIPE, 0, 0, false);
	}
	if (nk_button_label(context, "Save"))
	{
		save(editor);
	}
	if (nk_button_label(context, "Save as..."))
	{
		editor->save_pending = true;
		ask_file(editor, PICK_SAVE_RECIPE, 0, 0, true);
	}
	if (editor->message[0] != '\0')
	{
		nk_layout_row_dynamic(context, ROW * 2.0f, 1);
		nk_label_colored_wrap(context, editor->message, editor->message_is_error ? nk_rgb(255, 110, 90) : nk_rgb(150, 220, 150));
	}
	nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
	nk_layout_row_push(context, 0.22f);
	label(context, "Recipe");
	nk_layout_row_push(context, 0.78f);
	text_field(editor, context, editor->recipe_path, HOUSE_PATH_LENGTH);
	nk_layout_row_end(context);
	nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
	nk_layout_row_push(context, 0.22f);
	label(context, "Object");
	nk_layout_row_push(context, 0.78f);
	text_field(editor, context, recipe->output, HOUSE_PATH_LENGTH);
	nk_layout_row_end(context);
	nk_layout_row_dynamic(context, ROW, 1);
	label(context, recipe->output[0] == '\0' ? "  (none: objects/, named as the recipe)" : "  (under the data directory)");
	// A preset: the numbers of a sort of house
	nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
	nk_layout_row_push(context, 0.22f);
	label(context, "Preset");
	nk_layout_row_push(context, 0.53f);
	{
		const char *names[32];
		int count = SDL_min(house_preset_count(), 32);

		for (int p = 0; p < count; p++)
		{
			names[p] = house_preset_name(p);
		}
		editor->preset_index = nk_combo(context, names, count, editor->preset_index, (int)ROW, nk_vec2(240.0f, 360.0f));
	}
	nk_layout_row_push(context, 0.25f);
	if (nk_button_label(context, "Start from"))
	{
		house_preset(recipe, house_preset_name(editor->preset_index));
		editor->apply_asked = true;
	}
	nk_layout_row_end(context);
	nk_layout_row_dynamic(context, ROW, 1);
	{
		char text[96];

		snprintf(text, sizeof(text), "  Started from: %s", recipe->preset[0] != '\0' ? recipe->preset : "(no preset)");
		label(context, text);
	}
	// The kind and its size: they set every number at once
	{
		const char *kinds[HOUSE_KIND_COUNT];
		int kind;

		for (int k = 0; k < HOUSE_KIND_COUNT; k++)
		{
			kinds[k] = house_kind_name((house_kind_t)k);
		}
		kind = choose(context, "Kind", kinds, HOUSE_KIND_COUNT, (int)recipe->kind);
		recipe->kind = (house_kind_t)kind;
		nk_layout_row_dynamic(context, ROW, 2);
		whole(context, "Size", &recipe->size, 1, 3);
		if (nk_button_label(context, "Shape it as its kind"))
		{
			house_recipe_apply_kind(recipe);
		}
	}
	{
		const char *walls[HOUSE_WALL_COUNT];
		const char *roofs[HOUSE_ROOF_COUNT];

		for (int w = 0; w < HOUSE_WALL_COUNT; w++)
		{
			walls[w] = house_wall_name((house_wall_t)w);
		}
		for (int r = 0; r < HOUSE_ROOF_COUNT; r++)
		{
			roofs[r] = house_roof_name((house_roof_t)r);
		}
		recipe->wall = (house_wall_t)choose(context, "Walls", walls, HOUSE_WALL_COUNT, (int)recipe->wall);
		recipe->roof = (house_roof_t)choose(context, "Roof", roofs, HOUSE_ROOF_COUNT, (int)recipe->roof);
	}
	nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
	nk_layout_row_push(context, 0.6f);
	{
		int seed = (int)recipe->seed;

		nk_property_int(context, "Seed", 0, &seed, 1000000, 1, 0.2f);
		recipe->seed = (uint32_t)seed;
	}
	nk_layout_row_push(context, 0.2f);
	if (nk_button_label(context, "<"))
	{
		recipe->seed = recipe->seed > 0 ? recipe->seed - 1 : 0;
	}
	nk_layout_row_push(context, 0.2f);
	if (nk_button_label(context, ">"))
	{
		recipe->seed++;
	}
	nk_layout_row_end(context);
	nk_layout_row_dynamic(context, ROW, 1);
	whole(context, "Variants (next seeds, on Save)", &editor->variants, 0, 20);
}

static void body_section(house_recipe_t *recipe, struct nk_context *context)
{
	nk_layout_row_dynamic(context, ROW, 1);
	slider(context, "Width, along X (m)", &recipe->width, 2.0f, 20.0f, 0.1f);
	slider(context, "Depth, along Z (m)", &recipe->depth, 2.0f, 25.0f, 0.1f);
	whole(context, "Floors", &recipe->floors, 1, HOUSE_MAX_FLOORS);
	slider(context, "Floor height (m)", &recipe->floor_height, 1.8f, 4.5f, 0.05f);
	slider(context, "Base height (m)", &recipe->base_height, 0.0f, 2.0f, 0.02f);
	slider_scaled(context, "Wall thickness (cm)", &recipe->wall_thickness, 0.1f, 1.0f, 0.01f, 100.0f);
	slider_scaled(context, "Jetty: upper floors out (cm)", &recipe->jetty, 0.0f, 1.0f, 0.01f, 100.0f);
	check(context, "Jetty at the sides too", &recipe->jetty_sides);
	slider_scaled(context, "Gable out over top floor (cm)", &recipe->gable_jetty, 0.0f, 1.0f, 0.01f, 100.0f);
	check(context, "Masonry ground floor", &recipe->ground_floor_stone);
}

static void frame_section(house_recipe_t *recipe, struct nk_context *context)
{
	nk_layout_row_dynamic(context, ROW, 1);
	slider(context, "Post spacing (m)", &recipe->post_spacing, 0.6f, 4.0f, 0.05f);
	slider_scaled(context, "Post width (cm)", &recipe->post_width, 0.06f, 0.4f, 0.005f, 100.0f);
	slider_scaled(context, "Panels set back (cm)", &recipe->panel_recess, 0.0f, 0.1f, 0.002f, 100.0f);
	slider_scaled(context, "Chamfer (mm)", &recipe->bevel, 0.0f, 0.04f, 0.001f, 1000.0f);
	check(context, "Braces in the end bays", &recipe->braces);
	check(context, "Close studding", &recipe->studding);
	check(context, "Figures (crosses, bowed braces)", &recipe->figures);
	check(context, "Plaster tinted (by the seed)", &recipe->plaster_tint);
}

static void roof_section(house_recipe_t *recipe, struct nk_context *context)
{
	static const char *const dormer_kinds[3] = { "gabled", "shed", "either" };

	nk_layout_row_dynamic(context, ROW, 1);
	check(context, "Thatched", &recipe->thatch);
	slider(context, "Pitch (degrees)", &recipe->roof_pitch, 15.0f, 70.0f, 0.5f);
	slider_scaled(context, "Overhang (cm)", &recipe->overhang, 0.0f, 1.5f, 0.01f, 100.0f);
	slider_scaled(context, "Thickness (cm)", &recipe->roof_thickness, 0.03f, 0.4f, 0.005f, 100.0f);
	slider_scaled(context, "Sag (cm)", &recipe->roof_sag, 0.0f, 0.5f, 0.005f, 100.0f);
	slider_scaled(context, "Eave kick (cm)", &recipe->eave_kick, 0.0f, 0.4f, 0.005f, 100.0f);
	if (recipe->roof == HOUSE_ROOF_HALF_HIP)
	{
		slider(context, "Half hip: cut this far down", &recipe->hip_fraction, 0.05f, 0.95f, 0.01f);
	}
	check(context, "Chimney", &recipe->chimney);
	if (recipe->chimney)
	{
		slider_scaled(context, "Chimney width (cm)", &recipe->chimney_width, 0.3f, 2.0f, 0.01f, 100.0f);
	}
	whole(context, "Dormers per slope, at most", &recipe->dormers, 0, 6);
	if (recipe->dormers > 0)
	{
		slider(context, "Dormer width (m)", &recipe->dormer_width, 0.5f, 3.0f, 0.05f);
		recipe->dormer_kind = choose(context, "Dormer kind", dormer_kinds, 3, recipe->dormer_kind);
	}
}

static void openings_section(house_recipe_t *recipe, struct nk_context *context)
{
	nk_layout_row_dynamic(context, ROW, 1);
	check(context, "Windows", &recipe->windows);
	if (recipe->windows)
	{
		check(context, "Glazed (else plank shutters)", &recipe->glazed);
		check(context, "Shutters", &recipe->shutters);
		slider(context, "Window width (m)", &recipe->window_width, 0.3f, 2.5f, 0.05f);
		slider(context, "Window height (m)", &recipe->window_height, 0.3f, 2.5f, 0.05f);
		slider(context, "Sill height (m)", &recipe->sill_height, 0.2f, 1.8f, 0.05f);
		whole(context, "Lights in a band, at most", &recipe->window_lights, 1, 8);
		slider_scaled(context, "Light width (cm)", &recipe->light_width, 0.2f, 1.2f, 0.01f, 100.0f);
		check(context, "Oriel window (first floor)", &recipe->oriel);
	}
	slider(context, "Door width (m)", &recipe->door_width, 0.6f, 4.0f, 0.05f);
	slider(context, "Door height (m)", &recipe->door_height, 1.5f, 4.0f, 0.05f);
	check(context, "Loft door with a hoist beam", &recipe->loft_door);
	whole(context, "Slits per long wall", &recipe->slits, 0, 12);
	// The sign: none, the kind's, or an atlas cell from the signs on
	{
		const char *names[2 + HOUSE_CELL_COUNT];
		int count = 2;
		int value;

		names[0] = "none";
		names[1] = "by its kind";
		for (int cell = HOUSE_CELL_SIGN_INN; cell <= HOUSE_CELL_SIGN_FREE; cell++)
		{
			names[count++] = house_atlas_cell_name(cell);
		}
		value = recipe->sign == 0 ? 0 : (recipe->sign < 0 ? 1 : 2 + recipe->sign - HOUSE_CELL_SIGN_INN);
		value = choose(context, "Sign", names, count, value);
		recipe->sign = value == 0 ? 0 : (value == 1 ? -1 : HOUSE_CELL_SIGN_INN + value - 2);
	}
}

static void added_section(house_recipe_t *recipe, struct nk_context *context)
{
	nk_layout_row_dynamic(context, ROW, 1);
	check(context, "Porch (a covered walk in front)", &recipe->porch);
	if (recipe->porch)
	{
		slider(context, "Porch depth (m)", &recipe->porch_depth, 0.8f, 4.0f, 0.05f);
	}
	check(context, "Balcony on the top floor", &recipe->balcony);
	if (recipe->balcony)
	{
		slider(context, "Balcony depth (m)", &recipe->balcony_depth, 0.5f, 3.0f, 0.05f);
	}
	check(context, "Annex (lean-to on the right)", &recipe->annex);
}

static void age_section(house_recipe_t *recipe, struct nk_context *context)
{
	nk_layout_row_dynamic(context, ROW, 1);
	slider(context, "Wear (0 square .. 1 leaning)", &recipe->wear, 0.0f, 1.0f, 0.02f);
	slider(context, "Moss", &recipe->moss, 0.0f, 1.0f, 0.02f);
	slider(context, "Lichen", &recipe->lichen, 0.0f, 1.0f, 0.02f);
}

static void textures_section(editor_t *editor)
{
	static const char *const maps[4] = { "Color", "Normal", "ARM", "Emissive" };
	struct nk_context *context = &editor->ui.context;
	house_recipe_t *recipe = &editor->recipe;

	nk_layout_row_dynamic(context, ROW, 1);
	label(context, "  Under data/ (none: the stand-ins)");
	for (int m = 0; m < TEXTURED_MATERIALS; m++)
	{
		char text[64];

		nk_layout_row_dynamic(context, ROW, 1);
		snprintf(text, sizeof(text), "%s: a repeat (m)", house_material_name((house_material_kind_t)m));
		slider(context, text, &recipe->texture_meters[m], 0.1f, 8.0f, 0.05f);
		for (int k = 0; k < 4; k++)
		{
			nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
			nk_layout_row_push(context, 0.22f);
			label(context, maps[k]);
			nk_layout_row_push(context, 0.62f);
			text_field(editor, context, recipe->textures[m][k], HOUSE_PATH_LENGTH);
			nk_layout_row_push(context, 0.16f);
			if (nk_button_label(context, "..."))
			{
				ask_file(editor, PICK_TEXTURE, m, k, false);
			}
			nk_layout_row_end(context);
		}
	}
	nk_layout_row_dynamic(context, ROW, 2);
	if (nk_button_label(context, "Clear textures"))
	{
		memset(recipe->textures, 0, sizeof(recipe->textures));
	}
	if (nk_button_label(context, "Read them again"))
	{
		// (painted over while the editor runs)
		house_preview_forget_textures(&editor->preview);
		editor->apply_asked = true;
	}
}

static void view_section(editor_t *editor)
{
	static const char *const show_names[4] = { "as it is", "its sky (baked)", "its edges", "its shelter (baked)" };
	struct nk_context *context = &editor->ui.context;
	const char *hide_names[1 + HOUSE_MATERIAL_COUNT];

	nk_layout_row_dynamic(context, ROW, 1);
	label(context, "  F12 (or Print Screen): a screenshot, here");
	{
		nk_bool bake = editor->bake_wanted ? nk_true : nk_false;

		nk_checkbox_label(context, "Bake the weathering when it rests", &bake);
		editor->bake_wanted = bake != nk_false;
	}
	slider(context, "Sun, round (degrees)", &editor->sun_azimuth_degrees, -360.0f, 360.0f, 5.0f);
	slider(context, "Sun, up (degrees)", &editor->sun_elevation_degrees, 1.0f, 90.0f, 2.0f);
	slider(context, "Sky light (ambient)", &editor->ambient, 0.05f, 2.0f, 0.05f);
	slider(context, "Exposure (stops)", &editor->settings.render.exposure_ev, -6.0f, 6.0f, 0.25f);
	slider(context, "Wet (after the rain)", &editor->wetness, 0.0f, 1.0f, 0.05f);
	{
		char month_label[48];

		SDL_snprintf(month_label, sizeof(month_label), "Month (%s)", season_month_name(editor->month));
		slider(context, month_label, &editor->month, 1.0f, 12.99f, 0.25f);
	}
	slider(context, "Age, drawn (game: objects.ini)", &editor->age, 0.0f, 1.0f, 0.05f);
	slider(context, "Dirt up the walls (m)", &editor->dirt_meters, 0.0f, 2.0f, 0.05f);
	nk_property_int(context, "Detail level (0: all)", 0, &editor->detail, 3, 1, 0.2f);
	{
		nk_bool collision = editor->show_collision ? nk_true : nk_false;

		nk_checkbox_label(context, "The collision shell too", &collision);
		editor->show_collision = collision != nk_false;
	}
	editor->show = choose(context, "Show", show_names, 4, editor->show);
	hide_names[0] = "nothing";
	for (int m = 0; m < HOUSE_MATERIAL_COUNT; m++)
	{
		hide_names[m + 1] = house_material_name((house_material_kind_t)m);
	}
	editor->hide = choose(context, "Hide", hide_names, 1 + HOUSE_MATERIAL_COUNT, editor->hide);
}

static void panel(editor_t *editor)
{
	struct nk_context *context = &editor->ui.context;
	house_recipe_t *recipe = &editor->recipe;

	if (!nk_begin(context, "House", nk_rect(0.0f, 0.0f, PANEL_WIDTH, editor->ui.height), NK_WINDOW_BORDER | NK_WINDOW_TITLE))
	{
		nk_end(context);
		return;
	}
	// Apply: the house made again from what the panel says
	{
		bool changed = editor->grown_once && memcmp(&editor->recipe, &editor->grown_recipe, sizeof(house_recipe_t)) != 0;
		nk_bool automatic = editor->auto_apply ? nk_true : nk_false;

		nk_layout_row_dynamic(context, ROW, 2);
		if (nk_button_label(context, changed ? "Apply (F5) *" : "Apply (F5)"))
		{
			editor->apply_asked = true;
		}
		nk_checkbox_label(context, "At every change", &automatic);
		editor->auto_apply = automatic != nk_false;
	}
	if (editor->mesh_ok)
	{
		char text[256];
		int bake_state = SDL_GetAtomicInt(&editor->bake.state);

		nk_layout_row_dynamic(context, ROW, 1);
		snprintf(text, sizeof(text), "%zu triangles, %.1f x %.1f x %.1f m; %s", house_mesh_triangles(&editor->mesh, 0),
			(double)(editor->mesh.maximum[0] - editor->mesh.minimum[0]), (double)(editor->mesh.maximum[1] - editor->mesh.minimum[1]),
			(double)(editor->mesh.maximum[2] - editor->mesh.minimum[2]),
			editor->baked ? "baked" : (bake_state == BAKE_RUNNING ? "baking..." : "not baked"));
		label(context, text);
		snprintf(text, sizeof(text), "  farther: %zu, %zu, %zu triangles; made in %.0f ms", house_mesh_triangles(&editor->mesh, 1),
			house_mesh_triangles(&editor->mesh, 2), house_mesh_triangles(&editor->mesh, 3), editor->grow_seconds * 1000.0);
		label(context, text);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Recipe", NK_MAXIMIZED, 1))
	{
		recipe_section(editor);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Body", NK_MAXIMIZED, 2))
	{
		body_section(recipe, context);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Timber frame", NK_MINIMIZED, 3))
	{
		frame_section(recipe, context);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Roof", NK_MAXIMIZED, 4))
	{
		roof_section(recipe, context);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Windows and doors", NK_MINIMIZED, 5))
	{
		openings_section(recipe, context);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Added on", NK_MINIMIZED, 6))
	{
		added_section(recipe, context);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Age", NK_MINIMIZED, 7))
	{
		age_section(recipe, context);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Textures", NK_MINIMIZED, 8))
	{
		textures_section(editor);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "View", NK_MINIMIZED, 9))
	{
		view_section(editor);
		nk_tree_pop(context);
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------

static void view_of(const editor_t *editor, int width, int height, house_view_t *view)
{
	float yaw = editor->yaw_degrees * SDL_PI_F / 180.0f;
	float pitch = editor->pitch_degrees * SDL_PI_F / 180.0f;

	SDL_zerop(view);
	view->target[0] = editor->mesh_ok ? 0.5f * (editor->mesh.minimum[0] + editor->mesh.maximum[0]) : 0.0f;
	view->target[1] = editor->look_height;
	view->target[2] = editor->mesh_ok ? 0.5f * (editor->mesh.minimum[2] + editor->mesh.maximum[2]) : 0.0f;
	view->eye[0] = view->target[0] + SDL_cosf(pitch) * SDL_cosf(yaw) * editor->distance;
	view->eye[1] = view->target[1] + SDL_sinf(pitch) * editor->distance;
	view->eye[2] = view->target[2] + SDL_cosf(pitch) * SDL_sinf(yaw) * editor->distance;
	view->vertical_fov_degrees = 40.0f;
	view->aspect = (float)width / (float)SDL_max(height, 1);
	view->sun_azimuth_degrees = editor->sun_azimuth_degrees;
	view->sun_elevation_degrees = editor->sun_elevation_degrees;
	view->ambient = editor->ambient;
	view->month = editor->month;
	view->snow_first_month = SDL_roundf(editor->settings.render.snow_first_month);
	view->snow_last_month = SDL_roundf(editor->settings.render.snow_last_month);
	view->wetness = editor->wetness;
	view->detail = editor->detail;
	view->show_collision = editor->show_collision;
	view->hide = editor->hide > 0 ? house_material_name((house_material_kind_t)(editor->hide - 1)) : NULL;
	view->show = editor->show;
	view->age = editor->age;
	view->dirt_meters = editor->dirt_meters;
}

static void frame_house(editor_t *editor)
{
	float height = editor->mesh_ok ? editor->mesh.maximum[1] : 5.0f;
	float width = editor->mesh_ok ? SDL_max(editor->mesh.maximum[0] - editor->mesh.minimum[0], editor->mesh.maximum[2] - editor->mesh.minimum[2])
		: 8.0f;

	editor->look_height = SDL_max(height * 0.4f, 0.0f);
	editor->distance = SDL_max(SDL_max(height, width) * 2.0f, 2.0f);
}

int main(int argument_count, char **arguments)
{
	static editor_t editor;
	const char *screenshot_path = NULL;
	int screenshot_frames = 30;
	bool shoot_now = false;
	float fixed_distance = 0.0f;
	int frame_number = 0;
	bool running = true;
	bool save_first = false;
	int framed_kind = -1;
	char framed_preset[HOUSE_NAME_LENGTH] = "";

	editor.data_directory = "data";
	house_recipe_defaults(&editor.recipe);
	editor.yaw_degrees = 65.0f;
	editor.pitch_degrees = 14.0f;
	editor.distance = 20.0f;
	editor.look_height = 3.0f;
	editor.sun_azimuth_degrees = 125.0f;
	editor.sun_elevation_degrees = 42.0f;
	editor.ambient = 0.5f;
	editor.month = 7.5f;
	editor.age = 0.6f;
	editor.dirt_meters = 0.5f;
	editor.auto_apply = true;
	editor.bake_wanted = true;
	SDL_SetAtomicInt(&editor.bake.state, BAKE_IDLE);
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	editor.pick_lock = SDL_CreateMutex();
	for (int i = 1; i < argument_count; i++)
	{
		if (strcmp(arguments[i], "--data") == 0 && i + 1 < argument_count)
		{
			editor.data_directory = arguments[++i];
		}
		else if (strcmp(arguments[i], "--screenshot") == 0 && i + 1 < argument_count)
		{
			screenshot_path = arguments[++i];
		}
		else if (strcmp(arguments[i], "--preset") == 0 && i + 1 < argument_count)
		{
			if (!house_preset(&editor.recipe, arguments[++i]))
			{
				SDL_Log("%s", house_last_error());
			}
		}
		else if (strcmp(arguments[i], "--distance") == 0 && i + 1 < argument_count)
		{
			float distance = (float)SDL_atof(arguments[++i]);

			fixed_distance = SDL_clamp(distance, 0.5f, 300.0f);
		}
		else if (strcmp(arguments[i], "--yaw") == 0 && i + 1 < argument_count)
		{
			editor.yaw_degrees = (float)SDL_atof(arguments[++i]);
		}
		else if (strcmp(arguments[i], "--pitch") == 0 && i + 1 < argument_count)
		{
			float pitch = (float)SDL_atof(arguments[++i]);

			editor.pitch_degrees = SDL_clamp(pitch, -10.0f, 85.0f);
		}
		else if (strcmp(arguments[i], "--frames") == 0 && i + 1 < argument_count)
		{
			screenshot_frames = SDL_atoi(arguments[++i]);
			screenshot_frames = screenshot_frames > 0 ? screenshot_frames : 1;
		}
		else if (strcmp(arguments[i], "--save") == 0)
		{
			save_first = true;
		}
		else if (arguments[i][0] != '-')
		{
			open_recipe(&editor, arguments[i]);
		}
	}
	editor.window = SDL_CreateWindow("Potop house editor", 1500, 950, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (editor.window == NULL || !gpu_create(editor.window, screenshot_path == NULL, 8.0f))
	{
		SDL_Log("No window or no GPU: %s", SDL_GetError());
		return 1;
	}
	settings_set_defaults(&editor.settings);
	editor.settings.render.auto_exposure = false;
	editor.settings.render.exposure_ev = 0.0f;
	if (!post_create(&editor.post) || !house_preview_create(&editor.preview)
		|| !ui_backend_create(&editor.ui, editor.window, "ui/fonts/DejaVuSans.ttf", FONT_SIZE, 1.0f, 1.0f))
	{
		SDL_Log("The editor cannot start (shaders, font?): %s", SDL_GetError());
		return 1;
	}
	// (a ticked box green: nuklear's own dark mark on gray reads as empty)
	editor.ui.context.style.checkbox.cursor_normal = nk_style_item_color(nk_rgb(110, 200, 110));
	editor.ui.context.style.checkbox.cursor_hover = nk_style_item_color(nk_rgb(140, 230, 140));
	editor.last_seen = editor.recipe;
	// The preset's list shows what it started from
	for (int p = 0; p < house_preset_count(); p++)
	{
		if (strcmp(house_preset_name(p), editor.recipe.preset) == 0)
		{
			editor.preset_index = p;
		}
	}
	while (running)
	{
		SDL_Event event;
		int pixel_width = 0;
		int pixel_height = 0;
		int window_width = 0;
		int window_height = 0;

		ui_backend_input_begin(&editor.ui);
		while (SDL_PollEvent(&event))
		{
			bool interface = ui_backend_handle_event(&editor.ui, &event);

			if (event.type == SDL_EVENT_QUIT)
			{
				running = false;
			}
			if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
				&& (event.key.key == SDLK_F12 || event.key.key == SDLK_PRINTSCREEN))
			{
				shoot_now = true;
			}
			if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F5)
			{
				editor.apply_asked = true;
			}
			if (interface)
			{
				continue;
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			{
				editor.turning = editor.turning || event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_MIDDLE;
				editor.raising = editor.raising || event.button.button == SDL_BUTTON_RIGHT;
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
			{
				SDL_MouseButtonFlags held = SDL_GetMouseState(NULL, NULL);

				editor.turning = editor.turning && (held & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK)) != 0;
				editor.raising = editor.raising && event.button.button != SDL_BUTTON_RIGHT;
			}
			else if (event.type == SDL_EVENT_MOUSE_MOTION)
			{
				if (editor.turning)
				{
					editor.yaw_degrees += event.motion.xrel * 0.4f;
					editor.pitch_degrees = SDL_clamp(editor.pitch_degrees + event.motion.yrel * 0.3f, -10.0f, 85.0f);
				}
				if (editor.raising)
				{
					editor.look_height = SDL_max(editor.look_height + event.motion.yrel * editor.distance * 0.003f, 0.0f);
				}
			}
			else if (event.type == SDL_EVENT_MOUSE_WHEEL)
			{
				editor.distance = SDL_clamp(editor.distance * (event.wheel.y > 0.0f ? 0.9f : 1.1f), 0.5f, 300.0f);
			}
		}
		ui_backend_input_end(&editor.ui);
		take_answer(&editor);
		panel(&editor);
		ui_backend_update_capture(&editor.ui);
		grow_if_changed(&editor, screenshot_path != NULL);
		if (save_first && editor.mesh_ok)
		{
			save_first = false;
			save(&editor);
		}
		// The view fits the house once it is first made, and after the
		// kind or the preset change
		if (editor.mesh_ok && ((int)editor.recipe.kind != framed_kind || strcmp(framed_preset, editor.recipe.preset) != 0))
		{
			framed_kind = (int)editor.recipe.kind;
			snprintf(framed_preset, sizeof(framed_preset), "%s", editor.recipe.preset);
			frame_house(&editor);
			if (fixed_distance > 0.0f)
			{
				editor.distance = fixed_distance;
			}
		}

		SDL_GetWindowSizeInPixels(editor.window, &pixel_width, &pixel_height);
		SDL_GetWindowSize(editor.window, &window_width, &window_height);
		gpu_begin_frame();
		{
			float sky[4];
			int scene_width = 0;
			int scene_height = 0;
			SDL_GPURenderPass *pass;
			house_view_t view;

			house_preview_sky(sky);
			pass = render_target_begin(&editor.target, pixel_width, pixel_height, 1.0f, sky, &scene_width, &scene_height);
			if (pass != NULL)
			{
				float panel_pixels = PANEL_WIDTH * editor.ui.scale * (float)pixel_width / (float)SDL_max(window_width, 1);
				SDL_GPUViewport viewport = { panel_pixels, 0.0f, (float)scene_width - panel_pixels, (float)scene_height, 0.0f, 1.0f };

				SDL_SetGPUViewport(pass, &viewport);
				view_of(&editor, (int)viewport.w, scene_height, &view);
				house_preview_draw(&editor.preview, pass, &view);
				render_target_end(&editor.target, pass);
				post_draw(&editor.post, editor.target.color_texture, scene_width, scene_height, &editor.settings.render,
					(double)SDL_GetTicks() / 1000.0, screenshot_path != NULL);
			}
			ui_backend_render(&editor.ui, gpu.final_texture != NULL ? (int)gpu.final_width : pixel_width,
				gpu.final_texture != NULL ? (int)gpu.final_height : pixel_height);
			frame_number++;
			{
				bool shoot = screenshot_path != NULL && frame_number >= screenshot_frames;
				uint8_t *pixels = NULL;
				uint32_t shot_width = 0;
				uint32_t shot_height = 0;

				gpu_end_frame(shoot || shoot_now ? &pixels : NULL, &shot_width, &shot_height);
				if (shoot_now && pixels != NULL)
				{
					char name[128];

					screenshot_make_name(name, sizeof(name));
					if (screenshot_save(name, pixels, (int)shot_width, (int)shot_height))
					{
						say(&editor, false, "Saved the screenshot %s", name);
					}
					else
					{
						say(&editor, true, "Cannot write the screenshot %s", name);
					}
				}
				shoot_now = false;
				if (shoot)
				{
					screenshot_save(screenshot_path, pixels, (int)shot_width, (int)shot_height);
					running = false;
				}
				SDL_free(pixels);
			}
		}
	}
	// A baking still going: waited for, its house let go
	if (editor.bake.thread != NULL)
	{
		SDL_WaitThread(editor.bake.thread, NULL);
		if (SDL_GetAtomicInt(&editor.bake.state) == BAKE_DONE)
		{
			house_mesh_free(&editor.bake.mesh);
		}
	}
	house_mesh_free(&editor.mesh);
	ui_backend_destroy(&editor.ui);
	house_preview_destroy(&editor.preview);
	post_destroy(&editor.post);
	render_target_destroy(&editor.target);
	gpu_destroy();
	SDL_DestroyMutex(editor.pick_lock);
	SDL_DestroyWindow(editor.window);
	SDL_Quit();
	return 0;
}
