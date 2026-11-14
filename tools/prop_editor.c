// prop_editor: potop_prop in a window. The prop in view (drawn with the
// game's prop shaders, tools/prop_preview.c), made again from its recipe
// on Apply (or F5; "At every change": a moment after each change). A
// desktop tool: not part of the game, never shipped with it.
//
//   prop_editor [prop_plans/name.prop] [--data DIR]
//
// On the left, the recipe (see tools/prop_grow.h): a preset to start from,
// the kind and its seed, the textures, the numbers that shape it. Save
// writes the recipe (prop_plans/) and the object (.pobj, under
// data/objects/): the game, if it runs, shows the new prop at once.
// Variants writes the same prop with the next seeds too (name_1.pobj,
// name_2.pobj...), for a field of stones that does not look cloned.
//
// On the right, the prop: dragging with the left button (or the wheel's)
// turns round it, with the right one moves the eye up and down, the wheel
// comes nearer. F12 (or Print Screen): a screenshot.
//
// For the tests: --screenshot FILE [--frames N] draws N frames (30), writes
// what was on screen and leaves; --save saves first; --preset NAME starts
// from a preset; --yaw, --pitch, --distance (degrees, meters) the view;
// --month M, --wetness W, --show N (1 crevices, 2 edges, 3 sky).

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
#include "post.h"
#include "prop_grow.h"
#include "prop_preview.h"
#include "render_target.h"
#include "screenshot.h"
#include "season.h"
#include "settings.h"
#include "ui_backend.h"

#define PANEL_WIDTH 430.0f
#define FONT_SIZE 15.0f
#define ROW 24.0f
// A change waits this long (seconds) for the next before the prop is made
// again (dragging a slider: once it rests)
#define GROW_DELAY 0.15

typedef enum
{
	PICK_NONE,
	PICK_OPEN_RECIPE,
	PICK_SAVE_RECIPE,
	PICK_OUTPUT,
	PICK_TEXTURE,                      // which: pick_which (0 color, 1 normal, 2 arm)
	PICK_METAL_TEXTURE                 // the metal's (a barrel's hoops)
} pick_t;

typedef struct
{
	SDL_Window *window;
	ui_backend_t ui;
	settings_t settings;
	render_target_t target;
	post_t post;
	prop_preview_t preview;
	const char *data_directory;

	prop_recipe_t recipe;
	prop_recipe_t grown_recipe;
	prop_recipe_t last_seen;           // the recipe the frame before (a change's time)
	double changed_at;
	// Made again only on Apply (or F5), unless auto_apply: every change, a
	// moment after it (slow for the big ones: a wall takes half a second)
	bool apply_asked;
	bool auto_apply;
	bool grown_once;
	char recipe_path[PROP_PATH_LENGTH];
	prop_mesh_t mesh;
	bool mesh_ok;
	double grow_seconds;
	char message[1024];
	bool message_is_error;
	int preset_index;

	float yaw_degrees;
	float pitch_degrees;
	float distance;
	float look_height;
	float sun_azimuth_degrees;
	float sun_elevation_degrees;
	float ambient;
	float month;
	float wetness;
	bool pbr;
	int detail;
	int show;
	bool turning;
	bool raising;

	SDL_Mutex *pick_lock;
	pick_t pick_asked;
	int pick_which;
	bool pick_answered;
	char picked[PROP_PATH_LENGTH];
	int picked_count;
	int variants;
	int save_pending;
} editor_t;

static void save(editor_t *editor, int variants);

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

static void text_field(editor_t *editor, struct nk_context *context, char *text, int size)
{
	nk_flags state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, text, size, nk_filter_default);

	if (state & NK_EDIT_ACTIVE)
	{
		editor->ui.wants_text_input = true;
	}
}

static bool ends_with(const char *text, const char *end)
{
	size_t length = strlen(text);
	size_t end_length = strlen(end);

	return length >= end_length && strcmp(text + length - end_length, end) == 0;
}

// Whether a path is under the data directory (by the real paths)
static bool under_data(const editor_t *editor, const char *path)
{
	char folder[PROP_PATH_LENGTH];
	char real_folder[4096];
	char real_data[4096];
	const char *slash = strrchr(path, '/');
	size_t length;

	snprintf(folder, sizeof(folder), "%.*s", slash != NULL ? (int)(slash - path) : 1, slash != NULL ? path : ".");
	if (realpath(editor->data_directory, real_data) == NULL)
	{
		return true;
	}
	if (realpath(folder, real_folder) == NULL)
	{
		snprintf(real_folder, sizeof(real_folder), "%s", folder);
	}
	length = strlen(real_data);
	return strncmp(real_folder, real_data, length) == 0 && (real_folder[length] == '/' || real_folder[length] == '\0');
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

static void output_named_as_recipe(editor_t *editor)
{
	char name[PROP_PATH_LENGTH];
	char *dot;

	snprintf(name, sizeof(name), "%s", base_name(editor->recipe_path));
	dot = strrchr(name, '.');
	if (dot != NULL)
	{
		*dot = '\0';
	}
	if (name[0] != '\0')
	{
		snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s/objects/%s.pobj", editor->data_directory, name);
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
		relative_path(files[0], editor->picked, PROP_PATH_LENGTH);
		editor->picked_count = 1;
	}
	if (files == NULL)
	{
		editor->picked_count = -1;
		snprintf(editor->picked, PROP_PATH_LENGTH, "%s", SDL_GetError());
	}
	editor->pick_answered = true;
	SDL_UnlockMutex(editor->pick_lock);
}

static void ask_file(editor_t *editor, pick_t pick, int which, bool save)
{
	static const SDL_DialogFileFilter pictures[] = { { "Pictures", "png;jpg;jpeg" } };
	static const SDL_DialogFileFilter recipes[] = { { "Prop recipes", "prop" } };
	static const SDL_DialogFileFilter objects[] = { { "Objects", "pobj" } };
	const SDL_DialogFileFilter *filters = pick == PICK_OPEN_RECIPE || pick == PICK_SAVE_RECIPE ? recipes
		: (pick == PICK_OUTPUT ? objects : pictures);
	char *here = SDL_GetCurrentDirectory();

	if (editor->pick_asked != PICK_NONE)
	{
		SDL_free(here);
		return;
	}
	editor->pick_asked = pick;
	editor->pick_which = which;
	if (save)
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
	prop_recipe_t loaded;

	if (!prop_recipe_load(&loaded, path))
	{
		say(editor, true, "%s", prop_last_error());
		return;
	}
	editor->recipe = loaded;
	editor->apply_asked = true;
	snprintf(editor->recipe_path, sizeof(editor->recipe_path), "%s", path);
	say(editor, false, "Opened %s", path);
}

static void take_answer(editor_t *editor)
{
	char picked[PROP_PATH_LENGTH];
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
		editor->save_pending = -1;
		return;
	}
	switch (pick)
	{
		case PICK_OPEN_RECIPE:
			open_recipe(editor, picked);
			break;
		case PICK_SAVE_RECIPE:
			snprintf(editor->recipe_path, sizeof(editor->recipe_path), "%s", picked);
			if (!ends_with(editor->recipe_path, ".prop"))
			{
				SDL_strlcat(editor->recipe_path, ".prop", sizeof(editor->recipe_path));
			}
			output_named_as_recipe(editor);
			if (editor->save_pending >= 0)
			{
				int variants = editor->save_pending;

				editor->save_pending = -1;
				save(editor, variants);
			}
			break;
		case PICK_OUTPUT:
			snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s", picked);
			if (!ends_with(editor->recipe.output, ".pobj"))
			{
				SDL_strlcat(editor->recipe.output, ".pobj", sizeof(editor->recipe.output));
			}
			break;
		case PICK_TEXTURE:
		case PICK_METAL_TEXTURE:
		{
			char name[PROP_PATH_LENGTH];

			if (!texture_name(editor, picked, name, sizeof(name)))
			{
				say(editor, true, "'%s' is not under %s/: the game finds textures there only (copy it there first)", picked,
					editor->data_directory);
				break;
			}
			snprintf(pick == PICK_METAL_TEXTURE ? editor->recipe.metal_textures[editor->pick_which] : editor->recipe.textures[editor->pick_which],
				PROP_PATH_LENGTH, "%s", name);
			break;
		}
		case PICK_NONE:
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Making it

static void grow_if_changed(editor_t *editor, bool now)
{
	double seconds = (double)SDL_GetTicks() / 1000.0;

	if (memcmp(&editor->recipe, &editor->last_seen, sizeof(prop_recipe_t)) != 0)
	{
		editor->last_seen = editor->recipe;
		editor->changed_at = seconds;
	}
	if (editor->grown_once && memcmp(&editor->recipe, &editor->grown_recipe, sizeof(prop_recipe_t)) == 0)
	{
		return;
	}
	if (!now && editor->grown_once && !editor->apply_asked && (!editor->auto_apply || seconds - editor->changed_at < GROW_DELAY))
	{
		return;
	}
	editor->apply_asked = false;
	{
		prop_mesh_t mesh;
		uint64_t start = SDL_GetPerformanceCounter();

		editor->grown_recipe = editor->recipe;
		editor->grown_once = true;
		if (!prop_grow(&editor->recipe, editor->data_directory, &mesh))
		{
			say(editor, true, "%s", prop_last_error());
			return;
		}
		editor->grow_seconds = (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
		prop_mesh_free(&editor->mesh);
		editor->mesh = mesh;
		editor->mesh_ok = true;
		prop_preview_set_mesh(&editor->preview, &editor->mesh);
		prop_preview_set_textures(&editor->preview, &editor->mesh, editor->data_directory, false);
	}
}

static void variant_path(const char *output, int number, char *out, size_t out_size)
{
	size_t length = strlen(output);

	if (length > 5 && strcmp(output + length - 5, ".pobj") == 0)
	{
		snprintf(out, out_size, "%.*s_%d.pobj", (int)(length - 5), output, number);
	}
	else
	{
		snprintf(out, out_size, "%s_%d.pobj", output, number);
	}
}

static void save(editor_t *editor, int variants)
{
	grow_if_changed(editor, true);
	if (editor->recipe_path[0] == '\0')
	{
		editor->save_pending = variants;
		ask_file(editor, PICK_SAVE_RECIPE, 0, true);
		return;
	}
	if (!ends_with(editor->recipe_path, ".prop"))
	{
		SDL_strlcat(editor->recipe_path, ".prop", sizeof(editor->recipe_path));
	}
	if (editor->recipe.output[0] == '\0')
	{
		output_named_as_recipe(editor);
	}
	if (!editor->mesh_ok)
	{
		say(editor, true, "Nothing to save: %s", prop_last_error());
		return;
	}
	if (!under_data(editor, editor->recipe.output))
	{
		char moved[PROP_PATH_LENGTH];

		snprintf(moved, sizeof(moved), "%s/objects/%s", editor->data_directory, base_name(editor->recipe.output));
		say(editor, false, "'%s' is not under %s/: saved as %s", editor->recipe.output, editor->data_directory, moved);
		snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s", moved);
	}
	editor->grown_recipe = editor->recipe;
	if (!prop_recipe_save(&editor->recipe, editor->recipe_path) || !prop_write(editor->recipe.output, &editor->mesh))
	{
		say(editor, true, "%s", prop_last_error());
		return;
	}
	for (int v = 1; v <= variants; v++)
	{
		prop_recipe_t variant = editor->recipe;
		prop_mesh_t mesh;
		char path[PROP_PATH_LENGTH + 16];

		variant.seed = editor->recipe.seed + (unsigned)v;
		variant_path(editor->recipe.output, v, path, sizeof(path));
		if (!prop_grow(&variant, editor->data_directory, &mesh) || !prop_write(path, &mesh))
		{
			prop_mesh_free(&mesh);
			say(editor, true, "%s", prop_last_error());
			return;
		}
		prop_mesh_free(&mesh);
	}
	if (variants > 0)
	{
		say(editor, false, "Saved %s, %s and %d variants (seeds %u to %u)", editor->recipe_path, editor->recipe.output, variants,
			editor->recipe.seed + 1, editor->recipe.seed + (unsigned)variants);
	}
	else
	{
		say(editor, false, "Saved %s and %s", editor->recipe_path, editor->recipe.output);
	}
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

// The same, shown times scale (centimeters, millimeters: small numbers
// would show as 0.00, and a click's step would not show)
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

static const char *const style_names[] = { "granite", "sandstone", "slate", "limestone", "wood", "old_wood", "rusty_iron", "iron" };
#define STYLE_COUNT 8
static const char *const show_names[] = { "as it is", "its crevices", "its edges", "its sky" };

static void panel(editor_t *editor)
{
	struct nk_context *context = &editor->ui.context;
	prop_recipe_t *recipe = &editor->recipe;

	if (!nk_begin(context, "Prop", nk_rect(0.0f, 0.0f, PANEL_WIDTH, editor->ui.height), NK_WINDOW_BORDER | NK_WINDOW_TITLE))
	{
		nk_end(context);
		return;
	}
	// Apply: the prop made again from what the panel says
	{
		bool changed = editor->grown_once && memcmp(&editor->recipe, &editor->grown_recipe, sizeof(prop_recipe_t)) != 0;
		nk_bool automatic = editor->auto_apply ? nk_true : nk_false;

		nk_layout_row_dynamic(context, ROW, 2);
		if (nk_button_label(context, changed ? "Apply (F5) *" : "Apply (F5)"))
		{
			editor->apply_asked = true;
		}
		nk_checkbox_label(context, "At every change", &automatic);
		editor->auto_apply = automatic != nk_false;
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Recipe", NK_MAXIMIZED, 1))
	{
		nk_layout_row_dynamic(context, ROW, 4);
		if (nk_button_label(context, "New"))
		{
			prop_recipe_defaults(recipe);
			editor->recipe_path[0] = '\0';
			editor->apply_asked = true;
		}
		if (nk_button_label(context, "Open..."))
		{
			ask_file(editor, PICK_OPEN_RECIPE, 0, false);
		}
		if (nk_button_label(context, "Save"))
		{
			// (and its variants, as many as were saved last)
			save(editor, recipe->variants);
		}
		if (nk_button_label(context, "Save as..."))
		{
			editor->save_pending = recipe->variants;
			ask_file(editor, PICK_SAVE_RECIPE, 0, true);
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
		text_field(editor, context, editor->recipe_path, PROP_PATH_LENGTH);
		nk_layout_row_end(context);
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
		nk_layout_row_push(context, 0.22f);
		label(context, "Object");
		nk_layout_row_push(context, 0.62f);
		text_field(editor, context, recipe->output, PROP_PATH_LENGTH);
		nk_layout_row_push(context, 0.16f);
		if (nk_button_label(context, "..."))
		{
			ask_file(editor, PICK_OUTPUT, 0, true);
		}
		nk_layout_row_end(context);
		if (recipe->output[0] == '\0')
		{
			nk_layout_row_dynamic(context, ROW, 1);
			label(context, "  (none: data/objects/, named as the recipe)");
		}
		// A preset: the numbers of a sort of prop
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
		nk_layout_row_push(context, 0.22f);
		label(context, "Preset");
		nk_layout_row_push(context, 0.53f);
		{
			const char *names[32];
			int count = prop_preset_count();

			for (int p = 0; p < count && p < 32; p++)
			{
				names[p] = prop_preset_name(p);
			}
			editor->preset_index = nk_combo(context, names, count < 32 ? count : 32, editor->preset_index, (int)ROW, nk_vec2(220.0f, 330.0f));
		}
		nk_layout_row_push(context, 0.25f);
		if (nk_button_label(context, "Start from"))
		{
			prop_preset(recipe, prop_preset_name(editor->preset_index));
			editor->apply_asked = true;
		}
		nk_layout_row_end(context);
		nk_layout_row_dynamic(context, ROW, 2);
		label(context, recipe->preset[0] != '\0' ? recipe->preset : "(no preset)");
		{
			static const char *const kinds[PROP_KIND_COUNT] = { "rock", "wall", "fence", "barrel" };
			int kind = (int)recipe->kind;

			kind = nk_combo(context, kinds, PROP_KIND_COUNT, kind, (int)ROW, nk_vec2(200.0f, 120.0f));
			recipe->kind = (prop_kind_t)kind;
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
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Look", NK_MAXIMIZED, 2))
	{
		static const char *const maps[3] = { "Color", "Normal", "ARM" };

		nk_layout_row_dynamic(context, ROW, 1);
		label(context, "  Textures under data/ (none: the style's stand-ins)");
		for (int m = 0; m < 3; m++)
		{
			nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
			nk_layout_row_push(context, 0.2f);
			label(context, maps[m]);
			nk_layout_row_push(context, 0.64f);
			text_field(editor, context, recipe->textures[m], PROP_PATH_LENGTH);
			nk_layout_row_push(context, 0.16f);
			if (nk_button_label(context, "..."))
			{
				ask_file(editor, PICK_TEXTURE, m, false);
			}
			nk_layout_row_end(context);
		}
		nk_layout_row_dynamic(context, ROW, 2);
		label(context, "Stand-ins' style");
		{
			int style = 0;

			for (int s = 0; s < STYLE_COUNT; s++)
			{
				style = strcmp(recipe->style, style_names[s]) == 0 ? s : style;
			}
			style = nk_combo(context, style_names, STYLE_COUNT, style, (int)ROW, nk_vec2(200.0f, 240.0f));
			snprintf(recipe->style, sizeof(recipe->style), "%s", style_names[style]);
		}
		if (nk_button_label(context, "Clear textures"))
		{
			memset(recipe->textures, 0, sizeof(recipe->textures));
			memset(recipe->metal_textures, 0, sizeof(recipe->metal_textures));
		}
		if (nk_button_label(context, "Remake stand-ins"))
		{
			char names[3][PROP_PATH_LENGTH];

			if (prop_style_textures(recipe->style, editor->data_directory, true, names)
				&& (recipe->kind != PROP_KIND_BARREL || prop_style_textures(recipe->metal_style, editor->data_directory, true, names)))
			{
				prop_preview_set_textures(&editor->preview, &editor->mesh, editor->data_directory, true);
				say(editor, false, "Made %s/%s and its maps", editor->data_directory, names[0]);
			}
			else
			{
				say(editor, true, "%s", prop_last_error());
			}
		}
		// A barrel's iron: its own textures, or its style's
		if (recipe->kind == PROP_KIND_BARREL)
		{
			nk_layout_row_dynamic(context, ROW, 1);
			label(context, "  The iron (hoops, handle):");
			for (int m = 0; m < 3; m++)
			{
				nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
				nk_layout_row_push(context, 0.2f);
				label(context, maps[m]);
				nk_layout_row_push(context, 0.64f);
				text_field(editor, context, recipe->metal_textures[m], PROP_PATH_LENGTH);
				nk_layout_row_push(context, 0.16f);
				if (nk_button_label(context, "..."))
				{
					ask_file(editor, PICK_METAL_TEXTURE, m, false);
				}
				nk_layout_row_end(context);
			}
			nk_layout_row_dynamic(context, ROW, 2);
			label(context, "Its stand-ins' style");
			{
				int style = 6;

				for (int s = 0; s < STYLE_COUNT; s++)
				{
					style = strcmp(recipe->metal_style, style_names[s]) == 0 ? s : style;
				}
				style = nk_combo(context, style_names, STYLE_COUNT, style, (int)ROW, nk_vec2(200.0f, 240.0f));
				snprintf(recipe->metal_style, sizeof(recipe->metal_style), "%s", style_names[style]);
			}
		}
		nk_layout_row_dynamic(context, ROW, 1);
		slider(context, recipe->kind == PROP_KIND_FENCE || recipe->kind == PROP_KIND_BARREL ? "Wood: a repeat along (m)" : "A repeat (m)",
			&recipe->texture_meters, 0.1f, 8.0f, 0.05f);
		slider(context, "Moss", &recipe->moss, 0.0f, 1.0f, 0.02f);
		slider(context, "Lichen", &recipe->lichen, 0.0f, 1.0f, 0.02f);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Shape", NK_MAXIMIZED, 3))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		if (recipe->kind == PROP_KIND_ROCK)
		{
			slider(context, "Length (m)", &recipe->length, 0.02f, 20.0f, 0.02f);
			slider(context, "Height (m)", &recipe->height, 0.02f, 20.0f, 0.02f);
			slider(context, "Width (m)", &recipe->width, 0.02f, 20.0f, 0.02f);
			slider(context, "Boxiness", &recipe->boxiness, 0.0f, 1.0f, 0.02f);
			slider(context, "Lumps", &recipe->lumps, 0.0f, 1.0f, 0.01f);
			slider(context, "Lump size (m)", &recipe->lump_size, 0.02f, 10.0f, 0.02f);
			slider(context, "Grain", &recipe->grain, 0.0f, 1.0f, 0.02f);
			whole(context, "Split faces", &recipe->facets, 0, 16);
			slider(context, "Their depth", &recipe->facet_depth, 0.0f, 0.9f, 0.01f);
			slider(context, "Flat foot", &recipe->flat_bottom, 0.0f, 1.0f, 0.02f);
			slider(context, "Layers", &recipe->layers, 0.0f, 1.0f, 0.02f);
			slider(context, "Layer spacing (m)", &recipe->layer_spacing, 0.01f, 3.0f, 0.01f);
			slider(context, "Layer tilt (degrees)", &recipe->layer_tilt, 0.0f, 90.0f, 1.0f);
			whole(context, "Holes", &recipe->holes, 0, 4);
			slider(context, "Hole size", &recipe->hole_size, 0.05f, 0.8f, 0.01f);
			whole(context, "Stones (a heap)", &recipe->stones, 1, 64);
			if (recipe->stones > 1)
			{
				slider(context, "Heap's radius (m)", &recipe->spread, 0.01f, 20.0f, 0.02f);
			}
		}
		else if (recipe->kind == PROP_KIND_WALL)
		{
			slider(context, "Ring radius (m, 0: straight)", &recipe->ring_radius, 0.0f, 10.0f, 0.02f);
			if (recipe->ring_radius <= 0.05f)
			{
				slider(context, "Length (m)", &recipe->wall_length, 0.2f, 30.0f, 0.05f);
			}
			slider(context, "Height (m)", &recipe->wall_height, 0.1f, 5.0f, 0.02f);
			slider(context, "Thickness (m)", &recipe->wall_thickness, 0.1f, 3.0f, 0.02f);
			slider(context, "Narrower at its top", &recipe->batter, 0.0f, 0.6f, 0.01f);
			slider(context, "Stone size (m)", &recipe->stone_size, 0.05f, 2.0f, 0.01f);
			slider(context, "Ruin", &recipe->ruin, 0.0f, 1.0f, 0.02f);
			slider(context, "Stones: boxiness", &recipe->boxiness, 0.0f, 1.0f, 0.02f);
			slider(context, "Stones: lumps", &recipe->lumps, 0.0f, 1.0f, 0.01f);
			slider(context, "Stones: grain", &recipe->grain, 0.0f, 1.0f, 0.02f);
			whole(context, "Stones: split faces", &recipe->facets, 0, 3);
			slider(context, "Their depth", &recipe->facet_depth, 0.0f, 0.9f, 0.01f);
		}
		else if (recipe->kind == PROP_KIND_BARREL)
		{
			slider(context, "Height (m)", &recipe->barrel_height, 0.05f, 3.0f, 0.01f);
			slider_scaled(context, "Radius, middle (cm)", &recipe->barrel_radius, 0.03f, 2.0f, 0.005f, 100.0f);
			slider_scaled(context, "Radius, ends (cm)", &recipe->barrel_end_radius, 0.02f, 2.0f, 0.005f, 100.0f);
			whole(context, "Staves", &recipe->staves, 6, 48);
			slider_scaled(context, "Stave thickness (mm)", &recipe->stave_thickness, 0.004f, 0.1f, 0.001f, 1000.0f);
			whole(context, "Hoops", &recipe->hoops, 0, 8);
			slider_scaled(context, "Hoop width (cm)", &recipe->hoop_width, 0.005f, 0.2f, 0.002f, 100.0f);
			slider_scaled(context, "Hoop thickness (mm)", &recipe->hoop_thickness, 0.001f, 0.03f, 0.0005f, 1000.0f);
			{
				nk_bool lid = recipe->lid != 0 ? nk_true : nk_false;
				nk_bool lying = recipe->lying != 0 ? nk_true : nk_false;
				nk_bool handle = recipe->handle != 0 ? nk_true : nk_false;

				nk_checkbox_label(context, "A lid on its top", &lid);
				nk_checkbox_label(context, "Lying on its side", &lying);
				nk_checkbox_label(context, "A handle (a bucket)", &handle);
				recipe->lid = lid != nk_false ? 1 : 0;
				recipe->lying = lying != nk_false ? 1 : 0;
				recipe->handle = handle != nk_false ? 1 : 0;
			}
			slider(context, "Wear (1: broken)", &recipe->wear, 0.0f, 1.0f, 0.02f);
		}
		else
		{
			slider(context, "Length (m)", &recipe->fence_length, 0.3f, 30.0f, 0.05f);
			slider(context, "Height (m)", &recipe->fence_height, 0.2f, 4.0f, 0.02f);
			slider(context, "Post spacing (m)", &recipe->post_spacing, 0.2f, 10.0f, 0.05f);
			slider_scaled(context, "Post thickness (cm)", &recipe->post_thickness, 0.02f, 0.5f, 0.005f, 100.0f);
			whole(context, "Rails", &recipe->rails, 0, 6);
			slider_scaled(context, "Rail height (cm)", &recipe->rail_height, 0.01f, 0.5f, 0.005f, 100.0f);
			slider_scaled(context, "Rail thickness (cm)", &recipe->rail_depth, 0.01f, 0.3f, 0.005f, 100.0f);
			slider_scaled(context, "Picket spacing (cm, 0: none)", &recipe->picket_spacing, 0.0f, 1.0f, 0.005f, 100.0f);
			if (recipe->picket_spacing > 0.0f)
			{
				slider_scaled(context, "Picket width (cm)", &recipe->picket_width, 0.01f, 0.5f, 0.005f, 100.0f);
				slider_scaled(context, "Picket point (cm)", &recipe->picket_point, 0.0f, 0.5f, 0.005f, 100.0f);
			}
			slider(context, "Wear", &recipe->wear, 0.0f, 1.0f, 0.02f);
		}
		slider(context, "In the ground (m)", &recipe->sink, 0.0f, 3.0f, 0.01f);
		nk_tree_pop(context);
	}
	if (recipe->kind != PROP_KIND_FENCE && nk_tree_push_id(context, NK_TREE_TAB, "Detail", NK_MINIMIZED, 4))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		whole(context, recipe->kind == PROP_KIND_WALL ? "Cells along a stone" : "Cells along it", &recipe->resolution, 3,
			recipe->kind == PROP_KIND_WALL ? 40 : 400);
		whole(context, "Triangles, at most (0: all)", &recipe->triangles, 0, 200000);
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "View", NK_MINIMIZED, 5))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		label(context, "  F12 (or Print Screen): a screenshot, here");
		slider(context, "Sun, round (degrees)", &editor->sun_azimuth_degrees, -360.0f, 360.0f, 5.0f);
		slider(context, "Sun, up (degrees)", &editor->sun_elevation_degrees, 1.0f, 90.0f, 2.0f);
		slider(context, "Sky light (ambient)", &editor->ambient, 0.0f, 2.0f, 0.05f);
		slider(context, "Exposure (stops)", &editor->settings.render.exposure_ev, -6.0f, 6.0f, 0.25f);
		slider(context, "Wet (after the rain)", &editor->wetness, 0.0f, 1.0f, 0.05f);
		{
			char month_label[48];

			SDL_snprintf(month_label, sizeof(month_label), "Month (%s)", season_month_name(editor->month));
			slider(context, month_label, &editor->month, 1.0f, 12.99f, 0.25f);
			slider(context, "Snow from month", &editor->settings.render.snow_first_month, 1.0f, 12.0f, 1.0f);
			slider(context, "Snow to month", &editor->settings.render.snow_last_month, 1.0f, 12.0f, 1.0f);
		}
		{
			nk_bool pbr = editor->pbr ? nk_true : nk_false;

			nk_checkbox_label(context, "Physically based", &pbr);
			editor->pbr = pbr != nk_false;
		}
		nk_property_int(context, "Detail level (0: all)", 0, &editor->detail, 3, 1, 0.2f);
		nk_layout_row_dynamic(context, ROW, 2);
		label(context, "Show");
		editor->show = nk_combo(context, show_names, 4, editor->show, (int)ROW, nk_vec2(200.0f, 160.0f));
		nk_tree_pop(context);
	}
	if (nk_tree_push_id(context, NK_TREE_TAB, "Variants", NK_MINIMIZED, 6))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		label(context, "The next seeds too, as name_1.pobj, name_2.pobj...");
		nk_layout_row_dynamic(context, ROW, 2);
		nk_property_int(context, "How many", 1, &editor->variants, 50, 1, 0.2f);
		if (nk_button_label(context, "Save them"))
		{
			// (the recipe remembers how many: Save and potop_prop --all make them again)
			recipe->variants = editor->variants;
			editor->grown_recipe.variants = editor->variants;
			editor->last_seen.variants = editor->variants;
			save(editor, editor->variants);
		}
		nk_tree_pop(context);
	}
	if (editor->mesh_ok)
	{
		char text[256];

		nk_layout_row_dynamic(context, ROW, 1);
		snprintf(text, sizeof(text), "%zu triangles, %.2f x %.2f x %.2f m (%.0f ms)", prop_mesh_triangles(&editor->mesh, 0),
			(double)(editor->mesh.maximum[0] - editor->mesh.minimum[0]), (double)(editor->mesh.maximum[1] - editor->mesh.minimum[1]),
			(double)(editor->mesh.maximum[2] - editor->mesh.minimum[2]), editor->grow_seconds * 1000.0);
		label(context, text);
		if (prop_mesh_has_levels(&editor->mesh))
		{
			snprintf(text, sizeof(text), "  farther: %zu, %zu, %zu triangles", prop_mesh_triangles(&editor->mesh, 1),
				prop_mesh_triangles(&editor->mesh, 2), prop_mesh_triangles(&editor->mesh, 3));
			label(context, text);
		}
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------

static void view_of(const editor_t *editor, int width, int height, prop_view_t *view)
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
	view->pbr = editor->pbr;
	view->show = editor->show;
	view->detail = editor->detail;
	view->time_seconds = (float)((double)SDL_GetTicks() / 1000.0);
}

static void frame_prop(editor_t *editor)
{
	float height = editor->mesh_ok ? editor->mesh.maximum[1] : 1.0f;
	float width = editor->mesh_ok ? SDL_max(editor->mesh.maximum[0] - editor->mesh.minimum[0], editor->mesh.maximum[2] - editor->mesh.minimum[2])
		: 1.0f;

	editor->look_height = SDL_max(height * 0.4f, 0.0f);
	editor->distance = SDL_max(SDL_max(height, width) * 2.0f, 0.4f);
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
	char framed_preset[PROP_NAME_LENGTH] = "";

	editor.data_directory = "data";
	prop_recipe_defaults(&editor.recipe);
	editor.yaw_degrees = 35.0f;
	editor.pitch_degrees = 18.0f;
	editor.distance = 5.0f;
	editor.look_height = 0.5f;
	editor.sun_azimuth_degrees = 60.0f;
	editor.sun_elevation_degrees = 38.0f;
	editor.ambient = 0.35f;
	editor.month = 7.5f;
	editor.pbr = true;
	editor.variants = 4;
	editor.save_pending = -1;
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
			if (!prop_preset(&editor.recipe, arguments[++i]))
			{
				SDL_Log("%s", prop_last_error());
			}
		}
		else if (strcmp(arguments[i], "--month") == 0 && i + 1 < argument_count)
		{
			float month = (float)SDL_atof(arguments[++i]);

			editor.month = SDL_clamp(month, 1.0f, 12.99f);
		}
		else if (strcmp(arguments[i], "--wetness") == 0 && i + 1 < argument_count)
		{
			float wetness = (float)SDL_atof(arguments[++i]);

			editor.wetness = SDL_clamp(wetness, 0.0f, 1.0f);
		}
		else if (strcmp(arguments[i], "--detail") == 0 && i + 1 < argument_count)
		{
			int detail = SDL_atoi(arguments[++i]);

			editor.detail = SDL_clamp(detail, 0, 3);
		}
		else if (strcmp(arguments[i], "--show") == 0 && i + 1 < argument_count)
		{
			int show = SDL_atoi(arguments[++i]);

			editor.show = SDL_clamp(show, 0, 3);
		}
		else if (strcmp(arguments[i], "--distance") == 0 && i + 1 < argument_count)
		{
			float distance = (float)SDL_atof(arguments[++i]);

			fixed_distance = SDL_clamp(distance, 0.1f, 300.0f);
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
	editor.window = SDL_CreateWindow("Potop prop editor", 1500, 950, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (editor.window == NULL || !gpu_create(editor.window, screenshot_path == NULL, 8.0f))
	{
		SDL_Log("No window or no GPU: %s", SDL_GetError());
		return 1;
	}
	settings_set_defaults(&editor.settings);
	editor.settings.render.auto_exposure = false;
	editor.settings.render.exposure_ev = 0.0f;
	if (!post_create(&editor.post) || !prop_preview_create(&editor.preview)
		|| !ui_backend_create(&editor.ui, editor.window, "ui/fonts/DejaVuSans.ttf", FONT_SIZE, 1.0f, 1.0f))
	{
		SDL_Log("The editor cannot start (shaders, font?): %s", SDL_GetError());
		return 1;
	}
	editor.last_seen = editor.recipe;
	// The preset's list shows what it started from
	for (int p = 0; p < prop_preset_count(); p++)
	{
		if (strcmp(prop_preset_name(p), editor.recipe.preset) == 0)
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
				editor.distance = SDL_clamp(editor.distance * (event.wheel.y > 0.0f ? 0.9f : 1.1f), 0.2f, 300.0f);
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
			save(&editor, 0);
		}
		// The view fits the prop once it is first made, and after the
		// kind or the preset change
		if (editor.mesh_ok && ((int)editor.recipe.kind != framed_kind || strcmp(framed_preset, editor.recipe.preset) != 0))
		{
			framed_kind = (int)editor.recipe.kind;
			snprintf(framed_preset, sizeof(framed_preset), "%s", editor.recipe.preset);
			frame_prop(&editor);
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
			prop_view_t view;

			prop_preview_sky(sky);
			pass = render_target_begin(&editor.target, pixel_width, pixel_height, 1.0f, sky, &scene_width, &scene_height);
			if (pass != NULL)
			{
				float panel_pixels = PANEL_WIDTH * editor.ui.scale * (float)pixel_width / (float)SDL_max(window_width, 1);
				SDL_GPUViewport viewport = { panel_pixels, 0.0f, (float)scene_width - panel_pixels, (float)scene_height, 0.0f, 1.0f };

				SDL_SetGPUViewport(pass, &viewport);
				view_of(&editor, (int)viewport.w, scene_height, &view);
				prop_preview_draw(&editor.preview, pass, &view);
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
	prop_mesh_free(&editor.mesh);
	ui_backend_destroy(&editor.ui);
	prop_preview_destroy(&editor.preview);
	post_destroy(&editor.post);
	render_target_destroy(&editor.target);
	gpu_destroy();
	SDL_DestroyMutex(editor.pick_lock);
	SDL_DestroyWindow(editor.window);
	SDL_Quit();
	return 0;
}
