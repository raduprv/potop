// plant_editor: potop_plant in a window. The plant in view (drawn with the
// game's shaders, tools/plant_preview.c) while its recipe changes: every
// change grows it again at once. A desktop tool: not part of the game,
// never shipped with it.
//
//   plant_editor [plant_plans/name.plant] [--data DIR]
//
// On the left, the recipe (see tools/plant_grow.h): the kind and its seed,
// the pictures it is dressed in (picked with the system's file dialog or
// typed), the numbers that shape it. Save writes the recipe (plant_plans/,
// as sources), the atlas (only what changed) and the object (.pobj): the
// game, if it runs, shows the new plant at once. Variants writes the same
// plant with the next seeds too (name_1.pobj, name_2.pobj...), for a forest
// that does not look cloned. Until Save, new pictures are in the atlas in
// memory only. A picture's months (its pictures for parts of the year,
// name_m1.png .. name_m12.png beside it, or picked by hand): under its
// Months button.
//
// On the right, the plant: dragging with the left button (or the wheel's) turns round it,
// with the right one moves the eye up and down, the wheel comes nearer.
//
// For the tests: --screenshot FILE [--frames N] draws N frames (30), writes
// what was on screen and leaves; --save saves (as the button) first;
// --month M (1 .. 12.99) the month it is seen in, --detail N its level of
// detail.

// (realpath: POSIX)
#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gpu.h"
#include "plant_grow.h"
#include "plant_preview.h"
#include "post.h"
#include "render_target.h"
#include "screenshot.h"
#include "season.h"
#include "settings.h"
#include "ui_backend.h"

#define PANEL_WIDTH 430.0f
#define FONT_SIZE 15.0f
#define ROW 24.0f
// The fixed exposure's start (stops; the View section changes it)
#define EDITOR_EXPOSURE_EV 0.0f

// What a file dialog's answer is for
typedef enum
{
	PICK_NONE,
	PICK_OPEN_RECIPE,
	PICK_SAVE_RECIPE,
	PICK_OUTPUT,
	PICK_LEAVES,
	PICK_TWIGS,
	PICK_HEADS,
	PICK_FRUIT,
	PICK_PICTURE_MONTH,                // a picture's month's own (list, index and month below)
	PICK_BARK                          // which: 0 color, 1 normal, 2 arm
} pick_t;

#define MAX_PICKED 16

typedef struct
{
	SDL_Window *window;
	ui_backend_t ui;
	settings_t settings;
	render_target_t target;
	post_t post;
	plant_preview_t preview;
	const char *data_directory;

	plant_recipe_t recipe;
	plant_recipe_t grown_recipe;       // what the plant in view was grown from
	bool grown_once;
	char recipe_path[PLANT_PATH_LENGTH];
	plant_atlas_t *atlas;
	char *atlas_signature;             // its pictures and its name, as prepared
	char bark_signature[PLANT_PATH_LENGTH * 3 + 64];
	plant_mesh_t mesh;
	bool mesh_ok;
	char message[1024];
	bool message_is_error;

	// The view
	float yaw_degrees;
	float pitch_degrees;
	float distance;
	float look_height;
	float sun_azimuth_degrees;
	float sun_elevation_degrees;
	float wind;
	float ambient;
	int detail;                     // the level of detail shown (plant_view_t)
	float month;                    // the month shown (render.month, season.h; the snow's months: settings)
	float foliage_kept;
	bool turning;
	bool raising;

	// The file dialog: asked, and answered (from SDL's thread)
	SDL_Mutex *pick_lock;
	pick_t pick_asked;
	int pick_list;                     // the list (0 leaves .. 3 fruit) or the bark map
	int pick_index;                    // the picture
	int pick_month;                    // its month (0 January)
	bool pick_answered;
	char picked[MAX_PICKED][PLANT_PATH_LENGTH];
	int picked_count;

	// Text typed into the fields
	char typed[4][PLANT_PATH_LENGTH];  // a picture to add to each list
	// The picture whose months are shown (-1: none)
	int months_list;
	int months_index;
	int variants;
	int save_pending;                  // asked where the recipe goes, to save then: -1 none, else the variants
	int herb_preset;                   // the small plants' presets: the one picked
} editor_t;

static void save(editor_t *editor, int variants);

// ---------------------------------------------------------------------------
// Paths

// A path as seen from where the editor runs, if it is under it (the
// dialogs give absolute ones; recipes keep them relative)
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

// A text field; while it is being typed into, the keyboard's text comes
// to the interface (SDL's text input, which ui_backend starts only when
// asked: without it a field takes Backspace but no letters)
static void text_field(editor_t *editor, struct nk_context *context, char *text, int size)
{
	nk_flags state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, text, size, nk_filter_default);

	if (state & NK_EDIT_ACTIVE)
	{
		editor->ui.wants_text_input = true;
	}
}

// Whether the object's file is under the data directory (where the game
// looks), by the real paths of its folder and of the data directory
static bool output_under_data(const editor_t *editor)
{
	char folder[PLANT_PATH_LENGTH];
	char real_folder[4096];
	char real_data[4096];
	const char *slash = strrchr(editor->recipe.output, '/');
	size_t length;

	snprintf(folder, sizeof(folder), "%.*s", slash != NULL ? (int)(slash - editor->recipe.output) : 1,
		slash != NULL ? editor->recipe.output : ".");
	if (realpath(editor->data_directory, real_data) == NULL)
	{
		return true;
	}
	if (realpath(folder, real_folder) == NULL)
	{
		// (a folder not made yet: as written)
		snprintf(real_folder, sizeof(real_folder), "%s", folder);
	}
	length = strlen(real_data);
	return strncmp(real_folder, real_data, length) == 0 && (real_folder[length] == '/' || real_folder[length] == '\0');
}

// The object's file named as the recipe is (Save as: a new recipe, a new
// object beside the old one): its directory kept, data/objects/ if none
static void output_named_as_recipe(editor_t *editor)
{
	char name[PLANT_PATH_LENGTH];
	char directory[PLANT_PATH_LENGTH];
	char *dot;
	const char *slash = strrchr(editor->recipe.output, '/');

	snprintf(name, sizeof(name), "%s", base_name(editor->recipe_path));
	dot = strrchr(name, '.');
	if (dot != NULL)
	{
		*dot = '\0';
	}
	if (name[0] == '\0')
	{
		return;
	}
	if (slash != NULL && output_under_data(editor))
	{
		int length = (int)(slash - editor->recipe.output);

		snprintf(directory, sizeof(directory), "%.*s", length, editor->recipe.output);
	}
	else
	{
		snprintf(directory, sizeof(directory), "%s/objects", editor->data_directory);
	}
	snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s/%s.pobj", directory, name);
}

// ---------------------------------------------------------------------------
// File dialogs

static void SDLCALL dialog_answered(void *user_data, const char *const *files, int filter)
{
	editor_t *editor = user_data;

	(void)filter;
	SDL_LockMutex(editor->pick_lock);
	editor->picked_count = 0;
	for (int i = 0; files != NULL && files[i] != NULL && i < MAX_PICKED; i++)
	{
		relative_path(files[i], editor->picked[i], PLANT_PATH_LENGTH);
		editor->picked_count++;
	}
	// No list at all: the dialog could not be shown
	if (files == NULL)
	{
		editor->picked_count = -1;
		snprintf(editor->picked[0], PLANT_PATH_LENGTH, "%s", SDL_GetError());
	}
	editor->pick_answered = true;
	SDL_UnlockMutex(editor->pick_lock);
}

static void ask_file(editor_t *editor, pick_t pick, int list, int index, bool save, bool many)
{
	static const SDL_DialogFileFilter pictures[] = { { "Pictures", "png;jpg;jpeg" } };
	static const SDL_DialogFileFilter recipes[] = { { "Plant recipes", "plant" } };
	static const SDL_DialogFileFilter objects[] = { { "Objects", "pobj" } };
	const SDL_DialogFileFilter *filters = pick == PICK_OPEN_RECIPE || pick == PICK_SAVE_RECIPE ? recipes
		: (pick == PICK_OUTPUT ? objects : pictures);
	char *here = SDL_GetCurrentDirectory();

	if (editor->pick_asked != PICK_NONE)
	{
		return;
	}
	editor->pick_asked = pick;
	editor->pick_list = list;
	editor->pick_index = index;
	if (save)
	{
		SDL_ShowSaveFileDialog(dialog_answered, editor, editor->window, filters, 1, here);
	}
	else
	{
		SDL_ShowOpenFileDialog(dialog_answered, editor, editor->window, filters, 1, here, many);
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

static plant_pictures_t *list_of(plant_recipe_t *recipe, int list)
{
	plant_pictures_t *lists[4] = { &recipe->leaves, &recipe->twigs, &recipe->heads, &recipe->fruit };

	return lists[list >= 0 && list < 4 ? list : 0];
}

static void open_recipe(editor_t *editor, const char *path)
{
	plant_recipe_t loaded;

	if (!plant_recipe_load(&loaded, path))
	{
		say(editor, true, "%s", plant_last_error());
		return;
	}
	editor->recipe = loaded;
	snprintf(editor->recipe_path, sizeof(editor->recipe_path), "%s", path);
	say(editor, false, "Opened %s", path);
}

// The dialog's answer, once it came
static void take_answer(editor_t *editor)
{
	char picked[MAX_PICKED][PLANT_PATH_LENGTH];
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
		say(editor, true, "No file dialog (%s): zenity, or the desktop's portal, is needed for one; type the paths instead", picked[0]);
	}
	if (count <= 0)
	{
		editor->save_pending = -1;
		return;
	}
	switch (pick)
	{
		case PICK_OPEN_RECIPE:
			open_recipe(editor, picked[0]);
			break;
		case PICK_SAVE_RECIPE:
			snprintf(editor->recipe_path, sizeof(editor->recipe_path), "%s", picked[0]);
			if (strlen(editor->recipe_path) < 6 || strcmp(editor->recipe_path + strlen(editor->recipe_path) - 6, ".plant") != 0)
			{
				SDL_strlcat(editor->recipe_path, ".plant", sizeof(editor->recipe_path));
			}
			// A new recipe: a new object too, of its name
			output_named_as_recipe(editor);
			// What the recipe was asked for: saved now
			if (editor->save_pending >= 0)
			{
				int variants = editor->save_pending;

				editor->save_pending = -1;
				save(editor, variants);
			}
			break;
		case PICK_OUTPUT:
			snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s", picked[0]);
			if (strstr(editor->recipe.output, ".pobj") == NULL)
			{
				SDL_strlcat(editor->recipe.output, ".pobj", sizeof(editor->recipe.output));
			}
			break;
		case PICK_LEAVES:
		case PICK_TWIGS:
		case PICK_HEADS:
		case PICK_FRUIT:
			for (int i = 0; i < count; i++)
			{
				plant_pictures_add(list_of(&editor->recipe, pick - PICK_LEAVES), picked[i]);
			}
			break;
		case PICK_PICTURE_MONTH:
		{
			plant_pictures_t *list = list_of(&editor->recipe, editor->pick_list);

			if (editor->pick_index < list->count && editor->pick_month >= 0 && editor->pick_month < 12)
			{
				snprintf(list->pictures[editor->pick_index].months[editor->pick_month], PLANT_PATH_LENGTH, "%s", picked[0]);
			}
			break;
		}
		case PICK_BARK:
			snprintf(editor->recipe.bark[editor->pick_list], PLANT_PATH_LENGTH, "%s", picked[0]);
			break;
		case PICK_NONE:
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Growing

// Everything the atlas depends on: its name, the kind's (its atlas), the
// pictures and their months
static char *atlas_signature(const plant_recipe_t *recipe)
{
	size_t size = 1024;
	char *text;
	const plant_pictures_t *lists[4] = { &recipe->leaves, &recipe->twigs, &recipe->heads, &recipe->fruit };

	for (int l = 0; l < 4; l++)
	{
		size += (size_t)lists[l]->count * (PLANT_PATH_LENGTH * 13 + 16);
	}
	text = SDL_malloc(size);
	if (text == NULL)
	{
		return NULL;
	}
	snprintf(text, size, "%s|%s|%d", recipe->atlas, recipe->kind, plant_kind_find(recipe->kind) >= 0
		? (plant_kind_is_tree(plant_kind_find(recipe->kind)) ? 1 : 0) : -1);
	for (int l = 0; l < 4; l++)
	{
		for (int i = 0; i < lists[l]->count; i++)
		{
			const plant_picture_t *picture = &lists[l]->pictures[i];
			// Its months as they are found now (a picture put beside it shows)
			char sources[12][PLANT_PATH_LENGTH];
			bool own[12];

			plant_picture_months(picture, sources, own);
			SDL_strlcat(text, "\n", size);
			SDL_strlcat(text, picture->color, size);
			for (int m = 0; m < 12; m++)
			{
				SDL_strlcat(text, "|", size);
				SDL_strlcat(text, own[m] ? sources[m] : "", size);
			}
		}
	}
	// A mushroom with no texture of its own: the one made from its colors
	// (they and their patterns: made again when they change, not its shape)
	if (plant_kind_find(recipe->kind) >= 0 && plant_kind_is_mushroom(plant_kind_find(recipe->kind)) && recipe->leaves.count == 0)
	{
		const plant_mushroom_t *mushroom = &recipe->mushroom;
		char colors[512];

		snprintf(colors, sizeof(colors), "\n%s|%d|%g %g %g|%g %g %g|%g %g %g|%g %g %g|%g %g %g|%g %g %g|%g %g %g|%g %g %g %g %g %g %g %g %g",
			recipe->output, mushroom->underside, (double)mushroom->cap_color[0], (double)mushroom->cap_color[1], (double)mushroom->cap_color[2],
			(double)mushroom->cap_rim_color[0], (double)mushroom->cap_rim_color[1], (double)mushroom->cap_rim_color[2],
			(double)mushroom->cap_center_color[0], (double)mushroom->cap_center_color[1], (double)mushroom->cap_center_color[2],
			(double)mushroom->spot_color[0], (double)mushroom->spot_color[1], (double)mushroom->spot_color[2],
			(double)mushroom->gills_color[0], (double)mushroom->gills_color[1], (double)mushroom->gills_color[2],
			(double)mushroom->stem_color[0], (double)mushroom->stem_color[1], (double)mushroom->stem_color[2],
			(double)mushroom->stem_pattern_color[0], (double)mushroom->stem_pattern_color[1], (double)mushroom->stem_pattern_color[2],
			(double)mushroom->cap_center, (double)mushroom->spots, (double)mushroom->spot_size, (double)mushroom->scales,
			(double)mushroom->streaks, (double)mushroom->zones, (double)mushroom->stem_net, (double)mushroom->stem_bands, 0.0);
		SDL_strlcat(text, colors, size);
	}
	return text;
}

// Grows the plant again if the recipe changed (the atlas too, if its
// pictures did)
static void grow_if_changed(editor_t *editor)
{
	char *signature;
	char bark[sizeof(editor->bark_signature)];
	int kind = plant_kind_find(editor->recipe.kind);

	if (editor->grown_once && memcmp(&editor->recipe, &editor->grown_recipe, sizeof(editor->recipe)) == 0)
	{
		return;
	}
	editor->grown_recipe = editor->recipe;
	editor->grown_once = true;
	if (kind < 0)
	{
		say(editor, true, "No kind '%s'", editor->recipe.kind);
		return;
	}
	{
		float plain_color[3];

		plant_stalk_color(&editor->recipe, kind, plain_color);
		snprintf(bark, sizeof(bark), "%s|%s|%s|%.3f %.3f %.3f", editor->recipe.bark[0], editor->recipe.bark[1], editor->recipe.bark[2],
			(double)plain_color[0], (double)plain_color[1], (double)plain_color[2]);
		if (strcmp(bark, editor->bark_signature) != 0)
		{
			snprintf(editor->bark_signature, sizeof(editor->bark_signature), "%s", bark);
			plant_preview_set_bark(&editor->preview, editor->recipe.bark, plain_color, editor->data_directory);
		}
	}
	signature = atlas_signature(&editor->recipe);
	if (signature == NULL)
	{
		return;
	}
	if (editor->atlas == NULL || editor->atlas_signature == NULL || strcmp(signature, editor->atlas_signature) != 0)
	{
		uint64_t start = SDL_GetPerformanceCounter();

		plant_atlas_free(editor->atlas);
		editor->atlas = plant_atlas_prepare(&editor->recipe, editor->data_directory);
		SDL_free(editor->atlas_signature);
		editor->atlas_signature = signature;
		signature = NULL;
		if (editor->atlas == NULL)
		{
			say(editor, true, "%s", plant_last_error());
			plant_preview_set_mesh(&editor->preview, NULL);
			editor->mesh_ok = false;
			return;
		}
		plant_preview_set_atlas(&editor->preview, editor->atlas);
		SDL_Log("The %s atlas made in %.0f ms", plant_atlas_name(editor->atlas),
			1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());
	}
	SDL_free(signature);
	plant_mesh_free(&editor->mesh);
	editor->mesh_ok = plant_grow(&editor->recipe, editor->atlas, &editor->mesh);
	plant_preview_set_mesh(&editor->preview, editor->mesh_ok ? &editor->mesh : NULL);
	if (editor->mesh_ok)
	{
		if (editor->message_is_error)
		{
			editor->message[0] = '\0';
			editor->message_is_error = false;
		}
	}
	else
	{
		say(editor, true, "%s", plant_last_error());
	}
}

// The object's path with _N before .pobj
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
	grow_if_changed(editor);
	if (editor->recipe_path[0] == '\0')
	{
		// Where the recipe goes first; saved once that is answered
		editor->save_pending = variants;
		ask_file(editor, PICK_SAVE_RECIPE, 0, 0, true, false);
		return;
	}
	// A recipe is a .plant (typed without it: given it)
	{
		size_t length = strlen(editor->recipe_path);

		if (length < 6 || strcmp(editor->recipe_path + length - 6, ".plant") != 0)
		{
			SDL_strlcat(editor->recipe_path, ".plant", sizeof(editor->recipe_path));
		}
	}
	if (editor->recipe.output[0] == '\0')
	{
		// The object named as the recipe, in data/objects/
		const char *name = base_name(editor->recipe_path);
		const char *dot = strrchr(name, '.');

		snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s/objects/%.*s.pobj", editor->data_directory,
			dot != NULL ? (int)(dot - name) : (int)strlen(name), name);
	}
	if (!editor->mesh_ok || editor->atlas == NULL)
	{
		say(editor, true, "Nothing to save: %s", plant_last_error());
		return;
	}
	// The game finds objects under data/ only: one meant for elsewhere goes
	// in data/objects/, of its name
	if (!output_under_data(editor))
	{
		const char *name = base_name(editor->recipe.output);
		char moved[PLANT_PATH_LENGTH];

		snprintf(moved, sizeof(moved), "%s/objects/%s", editor->data_directory, name);
		say(editor, false, "'%s' is not under %s/: saved as %s", editor->recipe.output, editor->data_directory, moved);
		snprintf(editor->recipe.output, sizeof(editor->recipe.output), "%s", moved);
	}
	if (!plant_recipe_save(&editor->recipe, editor->recipe_path) || !plant_atlas_write(editor->atlas)
		|| !plant_write(editor->recipe.output, &editor->mesh, &editor->recipe, editor->atlas, editor->data_directory))
	{
		say(editor, true, "%s", plant_last_error());
		return;
	}
	for (int v = 1; v <= variants; v++)
	{
		plant_recipe_t variant = editor->recipe;
		plant_mesh_t mesh;
		char path[PLANT_PATH_LENGTH + 16];

		variant.seed = editor->recipe.seed + (unsigned)v;
		variant_path(editor->recipe.output, v, path, sizeof(path));
		if (!plant_grow(&variant, editor->atlas, &mesh) || !plant_write(path, &mesh, &variant, editor->atlas, editor->data_directory))
		{
			plant_mesh_free(&mesh);
			say(editor, true, "%s", plant_last_error());
			return;
		}
		plant_mesh_free(&mesh);
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

// A number the kind decides unless given: shown as the kind's until changed
static void float_setting(struct nk_context *context, const char *name, float *value, float kind_value, float minimum, float maximum,
	float step)
{
	float shown = *value > 0.0f ? *value : kind_value;
	float before = shown;

	nk_property_float(context, name, minimum, &shown, maximum, step, step * 0.5f);
	if (shown != before)
	{
		*value = shown;
	}
}

// The same for the settings where 0 means something (below 0: the kind's);
// shown times scale (small numbers would show as 0.01)
static void kind_setting(struct nk_context *context, const char *name, float *value, float kind_value, float minimum, float maximum,
	float step, float scale)
{
	float shown = (*value >= 0.0f ? *value : kind_value) * scale;
	float before = shown;

	nk_property_float(context, name, minimum * scale, &shown, maximum * scale, step * scale, step * scale * 0.5f);
	if (shown != before)
	{
		*value = shown / scale;
	}
}

// A small plant's preset to start from, beside its kind: the kind's own
// shape (the classic one) first, then the kind's presets (grass_clump;
// burdock, plantain...); the combo and its button, in the row begun
static void herb_preset_picker(struct nk_context *context, plant_recipe_t *recipe, int *preset_index)
{
	const char *names[33];
	int presets[33];
	int count = 1;

	names[0] = "(the kind's own)";
	presets[0] = -1;
	for (int p = 0; p < plant_herb_preset_count() && count < 33; p++)
	{
		if (strcmp(plant_herb_preset_kind(p), recipe->kind) == 0)
		{
			names[count] = plant_herb_preset_name(p);
			presets[count] = p;
			count++;
		}
	}
	if (*preset_index < 0 || *preset_index >= count)
	{
		*preset_index = 0;
	}
	*preset_index = nk_combo(context, names, count, *preset_index, (int)ROW, nk_vec2(220.0f, 260.0f));
	nk_layout_row_push(context, 0.26f);
	if (nk_button_label(context, "Start from"))
	{
		if (presets[*preset_index] < 0)
		{
			memset(&recipe->herb, 0, sizeof(recipe->herb));
			recipe->stalk_color[0] = -1.0f;
		}
		else
		{
			plant_herb_preset(recipe, plant_herb_preset_name(presets[*preset_index]));
		}
	}
}

// A small plant's shape (tuft, rosette, flower): its leaves round its foot,
// its flower stalks. Shown as the kind's own until one is changed: then the
// recipe gives them all
static void herb_section(struct nk_context *context, plant_recipe_t *recipe, int kind)
{
	plant_herb_t shown;
	plant_herb_t before;

	if (recipe->herb.given)
	{
		shown = recipe->herb;
	}
	else
	{
		plant_herb_kind_defaults(kind, &shown);
	}
	before = shown;
	nk_layout_row_dynamic(context, ROW, 1);
	label(context, recipe->herb.given ? "  Its own shape (bent strips, as below)" : "  The kind's own, the classic (change one: its own)");
	nk_property_int(context, "Leaves round its foot", 0, &shown.leaves, 60, 1, 0.2f);
	nk_property_float(context, "Leaves' sizes differ", 0.0f, &shown.leaf_spread, 1.0f, 0.02f, 0.01f);
	nk_property_float(context, "Leaves rise (degrees)", 0.0f, &shown.leaf_rise, 89.0f, 1.0f, 0.5f);
	nk_property_float(context, "  give or take (degrees)", 0.0f, &shown.leaf_rise_spread, 45.0f, 1.0f, 0.5f);
	nk_property_float(context, "Leaves arch over", 0.0f, &shown.leaf_arch, 3.0f, 0.02f, 0.01f);
	nk_property_float(context, "Inner ones smaller, steeper", 0.0f, &shown.leaf_layers, 1.0f, 0.02f, 0.01f);
	nk_property_float(context, "Leaf stalks (m, 0: none)", 0.0f, &shown.petiole, 2.0f, 0.01f, 0.005f);
	nk_property_float(context, "  they rise (degrees)", 0.0f, &shown.petiole_rise, 89.0f, 1.0f, 0.5f);
	if (shown.petiole > 0.0f)
	{
		float millimeters = shown.petiole_thickness * 1000.0f;

		nk_property_float(context, "  their radius (mm; 0: by the leaf)", 0.0f, &millimeters, 30.0f, 0.25f, 0.1f);
		shown.petiole_thickness = millimeters / 1000.0f;
	}
	nk_property_int(context, "Runners along the ground", 0, &shown.runners, 30, 1, 0.2f);
	if (shown.runners > 0)
	{
		float millimeters = shown.runner_thickness * 1000.0f;

		nk_property_float(context, "  their length (m)", 0.05f, &shown.runner_length, 5.0f, 0.02f, 0.01f);
		nk_property_int(context, "  each forks", 0, &shown.runner_forks, 4, 1, 0.2f);
		nk_property_float(context, "  they wind (0..1)", 0.0f, &shown.runner_wander, 1.0f, 0.02f, 0.01f);
		nk_property_float(context, "  their radius (mm; 0: 2)", 0.0f, &millimeters, 20.0f, 0.25f, 0.1f);
		shown.runner_thickness = millimeters / 1000.0f;
		nk_property_int(context, "  leaves (sprigs) along each", 0, &shown.runner_leaves, 60, 1, 0.2f);
		nk_property_float(context, "    size (of the leaf size; 0: 1)", 0.0f, &shown.runner_leaf_size, 3.0f, 0.02f, 0.01f);
		nk_property_float(context, "    rise (degrees; 0: 35)", 0.0f, &shown.runner_leaf_rise, 89.0f, 1.0f, 0.5f);
		nk_property_float(context, "    turn off it (degrees; 0: 50)", 0.0f, &shown.runner_leaf_turn, 90.0f, 1.0f, 0.5f);
	}
	nk_property_int(context, "Flower stalks, at least", 0, &shown.stalks_minimum, 30, 1, 0.2f);
	nk_property_int(context, "Flower stalks, at most", shown.stalks_minimum, &shown.stalks_maximum, 30, 1, 0.2f);
	nk_property_float(context, "  their height (of its)", 0.05f, &shown.stalk_height, 2.0f, 0.02f, 0.01f);
	nk_property_float(context, "  they lean (degrees)", 0.0f, &shown.stalk_lean, 80.0f, 1.0f, 0.5f);
	{
		float millimeters = shown.stalk_thickness * 1000.0f;

		nk_property_float(context, "  their radius (mm)", 0.5f, &millimeters, 30.0f, 0.25f, 0.1f);
		shown.stalk_thickness = millimeters / 1000.0f;
	}
	nk_property_int(context, "  leaves up each", 0, &shown.stalk_leaves, 60, 1, 0.2f);
	if (shown.stalk_leaves > 0)
	{
		nk_bool spiral = shown.stalk_leaf_spiral != 0 ? nk_true : nk_false;

		nk_property_float(context, "    from (of it; 0, 0: 0.1 to 0.6)", 0.0f, &shown.stalk_leaves_from, 1.0f, 0.02f, 0.01f);
		nk_property_float(context, "    to", 0.0f, &shown.stalk_leaves_to, 1.0f, 0.02f, 0.01f);
		nk_property_float(context, "    size (of the leaf size; 0: 0.55)", 0.0f, &shown.stalk_leaf_size, 3.0f, 0.02f, 0.01f);
		nk_property_float(context, "    smaller up it", 0.0f, &shown.stalk_leaf_taper, 1.0f, 0.02f, 0.01f);
		nk_property_float(context, "    rise (degrees; 0: 30)", -30.0f, &shown.stalk_leaf_rise, 89.0f, 1.0f, 0.5f);
		nk_property_float(context, "    the lowest hang (degrees lower)", 0.0f, &shown.stalk_leaf_droop, 120.0f, 1.0f, 0.5f);
		nk_property_float(context, "    arch over", 0.0f, &shown.stalk_leaf_arch, 3.0f, 0.02f, 0.01f);
		nk_property_float(context, "    their stalks (m)", 0.0f, &shown.stalk_petiole, 1.0f, 0.01f, 0.005f);
		if (shown.stalk_petiole > 0.0f)
		{
			float millimeters = shown.stalk_petiole_thickness * 1000.0f;

			nk_property_float(context, "      they rise (degrees; 0: 37)", 0.0f, &shown.stalk_petiole_rise, 89.0f, 1.0f, 0.5f);
			nk_property_float(context, "      radius (mm; 0: by the leaf)", 0.0f, &millimeters, 30.0f, 0.25f, 0.1f);
			shown.stalk_petiole_thickness = millimeters / 1000.0f;
		}
		nk_checkbox_label(context, "    round it one after the other", &spiral);
		shown.stalk_leaf_spiral = spiral != nk_false ? 1 : 0;
		if (shown.stalk_leaf_spiral)
		{
			nk_property_float(context, "      give or take (degrees)", 0.0f, &shown.stalk_leaf_scatter, 180.0f, 1.0f, 0.5f);
		}
	}
	nk_property_int(context, "  heads on each", 1, &shown.heads, 40, 1, 0.2f);
	nk_property_float(context, "  heads along its top (0..1)", 0.0f, &shown.head_spike, 1.0f, 0.02f, 0.01f);
	if (shown.heads > 1)
	{
		nk_property_float(context, "    the lower ones lean out (degrees)", 0.0f, &shown.head_lean, 85.0f, 1.0f, 0.5f);
	}
	nk_property_float(context, "  flower size (of its height)", 0.01f, &shown.head_size, 1.0f, 0.01f, 0.005f);
	{
		static const char *const shapes[3] = { "cards crossed", "disc on a cup", "disc" };

		nk_layout_row_dynamic(context, ROW, 2);
		label(context, "  the top head");
		shown.head_shape = nk_combo(context, shapes, 3, shown.head_shape < 0 || shown.head_shape > 2 ? 0 : shown.head_shape, (int)ROW,
			nk_vec2(200.0f, 120.0f));
		nk_layout_row_dynamic(context, ROW, 1);
	}
	if (shown.head_shape != PLANT_HEAD_CROSSED)
	{
		nk_property_float(context, "  it tilts, at least (degrees)", 0.0f, &shown.head_tilt_minimum, 85.0f, 1.0f, 0.5f);
		nk_property_float(context, "  it tilts, up to (degrees)", 0.0f, &shown.head_tilt, 85.0f, 1.0f, 0.5f);
		nk_property_float(context, "  its top: hollow -1 .. 1 domed", -1.0f, &shown.disc_curve, 1.0f, 0.05f, 0.01f);
		nk_property_float(context, "  the stalk bends into it (of it)", 0.0f, &shown.head_neck, 0.5f, 0.01f, 0.005f);
		{
			nk_bool to_light = shown.head_light > 0.0f ? nk_true : nk_false;

			nk_checkbox_label(context, "  it faces the light", &to_light);
			shown.head_light = to_light != nk_false ? 1.0f : 0.0f;
		}
	}
	if (shown.head_shape == PLANT_HEAD_CUP)
	{
		nk_property_float(context, "  cup's radius (of the disc's)", 0.05f, &shown.cup_size, 1.0f, 0.01f, 0.005f);
		nk_property_float(context, "  cup's depth (of its radius)", 0.05f, &shown.cup_depth, 2.0f, 0.02f, 0.01f);
		label(context, "  (from below: name_under.png beside the head's picture)");
	}
	// All the leaves: how they face (the light: the object's +Z, up 45
	// degrees; in the game turned only as far as the type's yaw_spread)
	label(context, "  All its leaves:");
	nk_property_float(context, "  folded along the middle", 0.0f, &shown.leaf_fold, 1.0f, 0.02f, 0.01f);
	nk_property_float(context, "  rolled at random (degrees)", 0.0f, &shown.leaf_twist, 90.0f, 1.0f, 0.5f);
	nk_property_float(context, "  face the light (0..1)", 0.0f, &shown.leaf_light, 1.0f, 0.02f, 0.01f);
	nk_property_float(context, "  the light's way, give or take (deg.)", 0.0f, &shown.light_spread, 180.0f, 1.0f, 0.5f);
	label(context, "  (the light: from +Z; objects.ini: yaw_spread)");
	if (memcmp(&shown, &before, sizeof(shown)) != 0)
	{
		shown.given = true;
		recipe->herb = shown;
	}
	if (recipe->herb.given && nk_button_label(context, "Back to the kind's own"))
	{
		memset(&recipe->herb, 0, sizeof(recipe->herb));
	}
}

// A mushroom's shape (of its height) and, if no texture is given, its colors
static void mushroom_color(struct nk_context *context, const char *name, float color[3])
{
	struct nk_colorf picked = { color[0], color[1], color[2], 1.0f };

	nk_layout_row_dynamic(context, ROW, 1);
	label(context, name);
	nk_layout_row_dynamic(context, 70.0f, 1);
	picked = nk_color_picker(context, picked, NK_RGB);
	color[0] = picked.r;
	color[1] = picked.g;
	color[2] = picked.b;
}

static void mushroom_section(struct nk_context *context, plant_mushroom_t *mushroom, bool colors)
{
	static const char *const undersides[3] = { "gills", "pores", "ridges" };

	nk_layout_row_dynamic(context, ROW, 2);
	label(context, "Start from");
	// (picking one sets all of it)
	if (nk_combo_begin_label(context, "a preset...", nk_vec2(220.0f, 320.0f)))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		for (int i = 0; i < plant_mushroom_preset_count(); i++)
		{
			if (nk_combo_item_label(context, plant_mushroom_preset_name(i), NK_TEXT_LEFT))
			{
				plant_mushroom_preset(mushroom, plant_mushroom_preset_name(i));
			}
		}
		nk_combo_end(context);
	}
	nk_layout_row_dynamic(context, ROW, 1);
	nk_property_float(context, "Size (times the height)", 0.1f, &mushroom->size, 10.0f, 0.05f, 0.02f);
	label(context, "  The stem (of the height):");
	nk_property_float(context, "Stem length (0: the rest)", 0.0f, &mushroom->stem_length, 2.0f, 0.02f, 0.01f);
	nk_property_float(context, "Stem thickness (radius)", 0.005f, &mushroom->stem_thickness, 0.4f, 0.005f, 0.002f);
	nk_property_float(context, "Stem top (of its foot)", 0.1f, &mushroom->stem_taper, 3.0f, 0.05f, 0.01f);
	nk_property_float(context, "Stem bulb at its foot", 0.0f, &mushroom->stem_bulb, 3.0f, 0.05f, 0.01f);
	nk_property_float(context, "Cup round its foot", 0.0f, &mushroom->stem_volva, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Stem bend", 0.0f, &mushroom->stem_bend, 1.5f, 0.05f, 0.01f);
	nk_property_float(context, "Skirt up the stem (0: none)", 0.0f, &mushroom->ring_height, 0.97f, 0.02f, 0.01f);
	nk_property_float(context, "Skirt width (of the stem)", 0.0f, &mushroom->ring_width, 3.0f, 0.05f, 0.01f);
	nk_property_float(context, "Skirt hangs", 0.0f, &mushroom->ring_droop, 1.5f, 0.05f, 0.01f);
	label(context, "  The cap:");
	nk_property_float(context, "Cap width (radius)", 0.02f, &mushroom->cap_width, 2.0f, 0.01f, 0.005f);
	nk_property_float(context, "Cap height", 0.01f, &mushroom->cap_height, 1.0f, 0.01f, 0.005f);
	nk_property_float(context, "Cone .. dome", 0.0f, &mushroom->cap_dome, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Funnel", 0.0f, &mushroom->cap_funnel, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Bump in the middle", 0.0f, &mushroom->cap_umbo, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Rim bent down (-: up)", -1.0f, &mushroom->cap_curl, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Rim wavy", 0.0f, &mushroom->cap_wave, 1.0f, 0.05f, 0.01f);
	nk_layout_row_dynamic(context, ROW, 2);
	label(context, "Under it");
	mushroom->underside = nk_combo(context, undersides, 3, mushroom->underside >= 0 && mushroom->underside < 3 ? mushroom->underside : 0,
		(int)ROW, nk_vec2(200.0f, 120.0f));
	if (!colors)
	{
		return;
	}
	nk_layout_row_dynamic(context, ROW, 1);
	label(context, "  Its colors (no texture given):");
	mushroom_color(context, "Cap", mushroom->cap_color);
	mushroom_color(context, "Cap, towards its rim", mushroom->cap_rim_color);
	mushroom_color(context, "Cap, its middle", mushroom->cap_center_color);
	nk_layout_row_dynamic(context, ROW, 1);
	nk_property_float(context, "Middle's color, how far", 0.0f, &mushroom->cap_center, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Rings of it", 0.0f, &mushroom->zones, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Streaks", 0.0f, &mushroom->streaks, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Warts", 0.0f, &mushroom->spots, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Warts' size", 0.0f, &mushroom->spot_size, 1.0f, 0.02f, 0.01f);
	nk_property_float(context, "Scales", 0.0f, &mushroom->scales, 1.0f, 0.05f, 0.01f);
	mushroom_color(context, "Warts and scales", mushroom->spot_color);
	mushroom_color(context, "Gills (pores, ridges)", mushroom->gills_color);
	mushroom_color(context, "Stem", mushroom->stem_color);
	mushroom_color(context, "Stem's net and bands", mushroom->stem_pattern_color);
	nk_layout_row_dynamic(context, ROW, 1);
	nk_property_float(context, "Stem's net", 0.0f, &mushroom->stem_net, 1.0f, 0.05f, 0.01f);
	nk_property_float(context, "Stem's bands", 0.0f, &mushroom->stem_bands, 1.0f, 0.05f, 0.01f);
}

static void pictures_section(editor_t *editor, struct nk_context *context, const char *title, int list, pick_t pick)
{
	plant_pictures_t *pictures = list_of(&editor->recipe, list);

	nk_layout_row_dynamic(context, ROW, 1);
	label(context, title);
	for (int i = 0; i < pictures->count; i++)
	{
		plant_picture_t *picture = &pictures->pictures[i];
		char text[PLANT_PATH_LENGTH + 32];

		bool open = editor->months_list == list && editor->months_index == i;
		char sources[12][PLANT_PATH_LENGTH];
		bool own[12];
		int own_count = 0;

		plant_picture_months(picture, sources, own);
		for (int m = 0; m < 12; m++)
		{
			own_count += own[m] ? 1 : 0;
		}
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 4);
		nk_layout_row_push(context, 0.44f);
		snprintf(text, sizeof(text), "  %s", base_name(picture->color));
		nk_label(context, text, NK_TEXT_LEFT);
		// Its cells in the atlas: as the kind has it, small or big (round)
		nk_layout_row_push(context, 0.16f);
		{
			static const char *const sizes[3] = { "kind", "small", "big" };
			int size = picture->cell_size >= PLANT_CELL_AS_KIND && picture->cell_size <= PLANT_CELL_BIG ? picture->cell_size : PLANT_CELL_AS_KIND;

			if (nk_button_label(context, sizes[size]))
			{
				picture->cell_size = (size + 1) % 3;
			}
		}
		nk_layout_row_push(context, 0.26f);
		snprintf(text, sizeof(text), "%s (%d)", open ? "Months -" : "Months +", own_count);
		if (nk_button_label(context, text))
		{
			editor->months_list = open ? -1 : list;
			editor->months_index = open ? -1 : i;
		}
		nk_layout_row_push(context, 0.14f);
		if (nk_button_label(context, "x"))
		{
			plant_pictures_remove(pictures, i);
			editor->months_list = -1;
			nk_layout_row_end(context);
			break;
		}
		nk_layout_row_end(context);
		// Its months: each its own (chosen, or found by name: name_m<month>
		// beside it), or the latest before it
		for (int m = 0; open && m < 12; m++)
		{
			static const char *const names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

			nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
			nk_layout_row_push(context, 0.66f);
			if (own[m])
			{
				snprintf(text, sizeof(text), "    %s: %s%s", names[m], base_name(sources[m]), picture->months[m][0] != '\0' ? " (chosen)" : "");
			}
			else
			{
				snprintf(text, sizeof(text), "    %s:   %s (%s)", names[m], strcmp(sources[m], picture->color) == 0 ? "the picture" : "as before",
					base_name(sources[m]));
			}
			nk_label_colored(context, text, NK_TEXT_LEFT, own[m] ? nk_rgb(210, 230, 210) : nk_rgb(140, 140, 140));
			nk_layout_row_push(context, 0.2f);
			if (nk_button_label(context, "Pick..."))
			{
				editor->pick_month = m;
				ask_file(editor, PICK_PICTURE_MONTH, list, i, false, false);
			}
			nk_layout_row_push(context, 0.14f);
			if (picture->months[m][0] != '\0' && nk_button_label(context, "x"))
			{
				picture->months[m][0] = '\0';
			}
			nk_layout_row_end(context);
		}
	}
	nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
	nk_layout_row_push(context, 0.62f);
	text_field(editor, context, editor->typed[list], PLANT_PATH_LENGTH);
	nk_layout_row_push(context, 0.18f);
	if (nk_button_label(context, "Add") && editor->typed[list][0] != '\0')
	{
		plant_pictures_add(pictures, editor->typed[list]);
		editor->typed[list][0] = '\0';
	}
	nk_layout_row_push(context, 0.2f);
	if (nk_button_label(context, "Pick..."))
	{
		ask_file(editor, pick, list, 0, false, true);
	}
	nk_layout_row_end(context);
}

static void panel(editor_t *editor)
{
	struct nk_context *context = &editor->ui.context;
	plant_recipe_t *recipe = &editor->recipe;
	int kind = plant_kind_find(recipe->kind);
	bool tree = kind >= 0 && plant_kind_is_tree(kind);
	bool mushroom = kind >= 0 && plant_kind_is_mushroom(kind);

	if (kind < 0)
	{
		kind = 0;
		snprintf(recipe->kind, sizeof(recipe->kind), "%s", plant_kind_name(0));
	}
	if (!nk_begin(context, "Plant", nk_rect(0.0f, 0.0f, PANEL_WIDTH, editor->ui.height), NK_WINDOW_BORDER | NK_WINDOW_TITLE))
	{
		nk_end(context);
		return;
	}
	// The recipe
	if (nk_tree_push_id(context, NK_TREE_TAB, "Recipe", NK_MAXIMIZED, 1))
	{
		nk_layout_row_dynamic(context, ROW, 4);
		if (nk_button_label(context, "New"))
		{
			plant_recipe_defaults(recipe);
			editor->recipe_path[0] = '\0';
		}
		if (nk_button_label(context, "Open..."))
		{
			ask_file(editor, PICK_OPEN_RECIPE, 0, 0, false, false);
		}
		if (nk_button_label(context, "Save"))
		{
			save(editor, 0);
		}
		if (nk_button_label(context, "Save as..."))
		{
			editor->save_pending = 0;
			ask_file(editor, PICK_SAVE_RECIPE, 0, 0, true, false);
		}
		// What happened last (saved, or what went wrong)
		if (editor->message[0] != '\0')
		{
			nk_layout_row_dynamic(context, ROW * 2.0f, 1);
			nk_label_colored_wrap(context, editor->message, editor->message_is_error ? nk_rgb(255, 110, 90) : nk_rgb(150, 220, 150));
		}
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
		nk_layout_row_push(context, 0.22f);
		label(context, "Recipe");
		nk_layout_row_push(context, 0.78f);
		text_field(editor, context, editor->recipe_path, PLANT_PATH_LENGTH);
		nk_layout_row_end(context);
		if (editor->recipe_path[0] == '\0')
		{
			nk_layout_row_dynamic(context, ROW, 1);
			label(context, "  (none yet: Save asks where, or type it)");
		}
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
		nk_layout_row_push(context, 0.22f);
		label(context, "Object");
		nk_layout_row_push(context, 0.6f);
		text_field(editor, context, recipe->output, PLANT_PATH_LENGTH);
		nk_layout_row_push(context, 0.18f);
		if (nk_button_label(context, "..."))
		{
			ask_file(editor, PICK_OUTPUT, 0, 0, true, false);
		}
		nk_layout_row_end(context);
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
		nk_layout_row_push(context, 0.22f);
		label(context, "Atlas");
		nk_layout_row_push(context, 0.78f);
		text_field(editor, context, recipe->atlas, (int)sizeof(recipe->atlas));
		nk_layout_row_end(context);
		nk_layout_row_dynamic(context, ROW, 1);
		label(context, recipe->atlas[0] != '\0' ? "  (data/textures/<atlas>.ini)" : "  (empty: the kind's)");
		nk_tree_pop(context);
	}
	// The kind and the seed
	if (nk_tree_push_id(context, NK_TREE_TAB, "Kind", NK_MAXIMIZED, 2))
	{
		const char *names[32];
		int count = plant_kind_count();
		int chosen = kind;
		int seed = (int)recipe->seed;
		char text[64];

		for (int k = 0; k < count && k < 32; k++)
		{
			names[k] = plant_kind_name(k);
		}
		// A small plant's: its preset beside it
		if (strcmp(recipe->kind, "tuft") == 0 || strcmp(recipe->kind, "rosette") == 0 || strcmp(recipe->kind, "flower") == 0)
		{
			nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
			nk_layout_row_push(context, 0.37f);
			chosen = nk_combo(context, names, count, chosen, (int)ROW, nk_vec2(PANEL_WIDTH - 40.0f, 400.0f));
			nk_layout_row_push(context, 0.37f);
			herb_preset_picker(context, recipe, &editor->herb_preset);
			nk_layout_row_end(context);
		}
		else
		{
			nk_layout_row_dynamic(context, ROW, 1);
			chosen = nk_combo(context, names, count, chosen, (int)ROW, nk_vec2(PANEL_WIDTH - 40.0f, 400.0f));
		}
		if (chosen != kind)
		{
			snprintf(recipe->kind, sizeof(recipe->kind), "%s", plant_kind_name(chosen));
			editor->herb_preset = 0;
			// The kind's own shape, not the last one's
			recipe->branch_levels = 0;
			recipe->leaves_per_branch = -1;
		}
		nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
		nk_layout_row_push(context, 0.75f);
		nk_property_int(context, "Seed", 1, &seed, 1000000, 1, 0.2f);
		nk_layout_row_push(context, 0.25f);
		if (nk_button_label(context, "Dice"))
		{
			seed = (int)(SDL_rand(999999) + 1);
		}
		nk_layout_row_end(context);
		recipe->seed = (unsigned)seed;
		nk_layout_row_dynamic(context, ROW, 1);
		nk_property_float(context, "Height (0: the kind's)", 0.0f, &recipe->height, 60.0f, 0.1f, 0.05f);
		snprintf(text, sizeof(text), "  grown: %.2f m (the kind: about %.1f)", editor->mesh_ok ? (double)editor->mesh.height : 0.0,
			(double)plant_kind_height(kind));
		label(context, text);
		nk_tree_pop(context);
	}
	// The shape
	if (nk_tree_push_id(context, NK_TREE_TAB, "Shape", NK_MAXIMIZED, 3))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		float_setting(context, "Leaf size (m)", &recipe->leaf_size, plant_kind_leaf_size(kind), 0.01f, 3.0f, 0.01f);
		// A small plant's own shape (burdock, plantain...)
		if (strcmp(recipe->kind, "tuft") == 0 || strcmp(recipe->kind, "rosette") == 0 || strcmp(recipe->kind, "flower") == 0)
		{
			herb_section(context, recipe, kind);
		}
		// A fern's fronds: how steep they start, how far they arch over
		if (strcmp(recipe->kind, "fern") == 0)
		{
			kind_setting(context, "Fronds start (degrees up)", &recipe->frond_angle, 58.0f, 0.0f, 89.0f, 1.0f, 1.0f);
			kind_setting(context, "Fronds arch over", &recipe->frond_droop, 0.7f, 0.0f, 3.0f, 0.05f, 1.0f);
		}
		if (tree)
		{
			int levels = plant_kind_levels(kind);
			int leaves = recipe->leaves_per_branch >= 0 ? recipe->leaves_per_branch : plant_kind_leaves_per_branch(kind);
			int before = leaves;
			float length = recipe->branch_length > 0.0f ? recipe->branch_length : 1.0f;

			for (int level = 0; level < levels; level++)
			{
				int value = level < recipe->branch_levels ? recipe->branches[level] : plant_kind_branches(kind, level);
				int was = value;
				char name[64];

				snprintf(name, sizeof(name), level == 0 ? "Branches on the trunk" : "Branches on those (level %d)", level + 1);
				nk_property_int(context, name, 0, &value, 100, 1, 0.2f);
				if (value != was)
				{
					// All the levels given from now on
					for (int k = recipe->branch_levels; k < levels; k++)
					{
						recipe->branches[k] = plant_kind_branches(kind, k);
					}
					recipe->branch_levels = levels;
					recipe->branches[level] = value;
				}
			}
			nk_property_int(context, "Leaves per finest branch", 0, &leaves, 200, 1, 0.2f);
			if (leaves != before)
			{
				recipe->leaves_per_branch = leaves;
			}
			nk_property_float(context, "Branch length (times)", 0.2f, &length, 4.0f, 0.05f, 0.01f);
			recipe->branch_length = length != 1.0f ? length : 0.0f;
			for (int level = 1; level < levels && level < PLANT_MAX_LEVELS; level++)
			{
				float level_length = recipe->branch_lengths[level] > 0.0f ? recipe->branch_lengths[level] : 1.0f;
				char name[64];

				snprintf(name, sizeof(name), "Level %d length (times)", level + 1);
				nk_property_float(context, name, 0.2f, &level_length, 4.0f, 0.05f, 0.01f);
				recipe->branch_lengths[level] = level_length != 1.0f ? level_length : 0.0f;
			}
			float_setting(context, "Twig scale", &recipe->twig_scale, plant_kind_twig_scale(kind), 0.2f, 5.0f, 0.05f);
			kind_setting(context, "Twigs in a row past (m; 0: one)", &recipe->twig_meters, plant_kind_twig_meters(kind), 0.0f, 5.0f, 0.05f, 1.0f);
			nk_property_float(context, "Twig share (with leaves)", 0.0f, &recipe->twig_share, 1.0f, 0.05f, 0.01f);
			nk_property_float(context, "Twig size (m; 0: the branch's)", 0.0f, &recipe->twig_size, 2.0f, 0.01f, 0.005f);
			nk_property_float(context, "Twigs per meter (0: the kind's)", 0.0f, &recipe->twig_density, 200.0f, 0.5f, 0.1f);
			nk_property_float(context, "Longest bare stretch (m; 0: any)", 0.0f, &recipe->twig_gap, 2.0f, 0.01f, 0.005f);
			nk_property_float(context, "Branches flat (off lying ones)", 0.0f, &recipe->branch_flat, 1.0f, 0.05f, 0.01f);
			{
				// (a test: the fir's alone, for now)
				nk_bool curved = recipe->curved_twigs ? nk_true : nk_false;

				nk_checkbox_label(context, "Curved twigs (fir only, test)", &curved);
				recipe->curved_twigs = curved != 0;
			}
			{
				// (pines: twig pictures one brush side on, a head picture it end on)
				nk_bool brushes = recipe->brushes ? nk_true : nk_false;

				nk_checkbox_label(context, "Brushes in the round (pines)", &brushes);
				recipe->brushes = brushes != 0;
				if (recipe->brushes)
				{
					nk_property_int(context, "  strips a cone", 3, &recipe->brush_strips, 32, 1, 0.2f);
					nk_property_float(context, "  cones apart (needles)", 0.1f, &recipe->brush_spacing, 2.0f, 0.05f, 0.01f);
				}
			}
			{
				// (whole boughs: fir_squares)
				nk_bool baked = recipe->snow_bake ? nk_true : nk_false;

				nk_checkbox_label(context, "Snow baked in (whole boughs)", &baked);
				recipe->snow_bake = baked != 0;
			}
			nk_property_float(context, "Loose leaves per meter", 0.0f, &recipe->loose_leaves, 100.0f, 0.5f, 0.1f);
			kind_setting(context, "Crown starts at (% of height)", &recipe->crown_start, plant_kind_crown_start(kind), 0.0f, 0.95f, 0.01f, 100.0f);
			kind_setting(context, "Leaves from (% of height)", &recipe->leaves_from, plant_kind_leaves_from(kind), 0.0f, 0.95f, 0.01f, 100.0f);
			kind_setting(context, "Dead branches under the crown", &recipe->dead_branches, plant_kind_dead_branches(kind), 0.0f, 60.0f, 1.0f, 1.0f);
			kind_setting(context, "Branches droop (x100)", &recipe->droop, plant_kind_droop(kind), 0.0f, 0.5f, 0.005f, 100.0f);
			kind_setting(context, "Tips turn up", &recipe->tip_lift, plant_kind_tip_lift(kind), 0.0f, 3.0f, 0.05f, 1.0f);
			kind_setting(context, "Branch collar", &recipe->collar, plant_kind_collar(kind), 0.0f, 3.0f, 0.05f, 1.0f);
			kind_setting(context, "Root flare", &recipe->root_flare, plant_kind_root_flare(kind), 0.0f, 3.0f, 0.05f, 1.0f);
			// How thick the trunk and the branches are
			kind_setting(context, "Trunk thickness (% of height)", &recipe->trunk_thickness, plant_kind_trunk_thickness(kind), 0.001f, 0.2f,
				0.0005f, 100.0f);
			kind_setting(context, "Trunk top (% of its foot)", &recipe->trunk_taper, plant_kind_trunk_taper(kind), 0.01f, 1.0f, 0.01f, 100.0f);
			kind_setting(context, "Branch thickness (% of parent)", &recipe->branch_thickness, plant_kind_branch_thickness(kind), 0.01f, 1.0f,
				0.01f, 100.0f);
			kind_setting(context, "Branch tip (% of its foot)", &recipe->branch_taper, plant_kind_branch_taper(kind), 0.01f, 1.0f, 0.01f, 100.0f);
			kind_setting(context, "Thinnest branch (mm)", &recipe->thinnest_branch, plant_kind_thinnest_branch(kind), 0.0005f, 0.1f, 0.0005f,
				1000.0f);
			// The trunk: stems grown together, gnarled, bent by the wind
			{
				int stems = recipe->stems >= 1 ? recipe->stems : plant_kind_stems(kind);
				int stems_before = stems;

				nk_property_int(context, "Stems", 1, &stems, 12, 1, 0.2f);
				if (stems != stems_before)
				{
					recipe->stems = stems;
				}
			}
			kind_setting(context, "Stems part at (% of height)", &recipe->fork_height, plant_kind_fork_height(kind), 0.0f, 0.9f, 0.01f, 100.0f);
			kind_setting(context, "One stem more (% chance)", &recipe->extra_stem_chance, plant_kind_extra_stem_chance(kind), 0.0f, 1.0f, 0.01f,
				100.0f);
			kind_setting(context, "Trunk upright (straight)", &recipe->trunk_upright, plant_kind_trunk_upright(kind), 0.0f, 3.0f, 0.05f, 1.0f);
			kind_setting(context, "Lit from below the middle (mats)", &recipe->light_from_below, plant_kind_light_from_below(kind), 0.0f, 10.0f, 0.05f, 1.0f);
			kind_setting(context, "Gnarl", &recipe->gnarl, plant_kind_gnarl(kind), 0.0f, 3.0f, 0.05f, 1.0f);
			kind_setting(context, "Kinks", &recipe->kinks, plant_kind_kinks(kind), 0.0f, 2.0f, 0.05f, 1.0f);
			kind_setting(context, "Wind: lean (degrees)", &recipe->lean_degrees, plant_kind_lean_degrees(kind), 0.0f, 88.0f, 1.0f, 1.0f);
			kind_setting(context, "Wind: lean for (% of trunk)", &recipe->lean_length, plant_kind_lean_length(kind), 0.0f, 1.0f, 0.01f, 100.0f);
			kind_setting(context, "Wind: straighten", &recipe->straighten, plant_kind_straighten(kind), 0.0f, 1.0f, 0.05f, 1.0f);
			kind_setting(context, "Wind: windward side bare", &recipe->wind_side, plant_kind_wind_side(kind), 0.0f, 1.0f, 0.05f, 1.0f);
		}
		nk_property_float(context, "Bark tile (m)", 0.05f, &recipe->bark_meters, 10.0f, 0.05f, 0.01f);
		nk_layout_row_dynamic(context, ROW, 1);
		if (nk_button_label(context, "The kind's shape again"))
		{
			recipe->leaf_size = 0.0f;
			recipe->leaves_per_branch = -1;
			recipe->branch_levels = 0;
			recipe->branch_length = 0.0f;
			memset(recipe->branch_lengths, 0, sizeof(recipe->branch_lengths));
			recipe->twig_scale = 0.0f;
			recipe->crown_start = -1.0f;
			recipe->leaves_from = -1.0f;
			recipe->dead_branches = -1.0f;
			recipe->trunk_upright = -1.0f;
			recipe->light_from_below = -1.0f;
			recipe->droop = -1.0f;
			recipe->trunk_thickness = -1.0f;
			recipe->trunk_taper = -1.0f;
			recipe->branch_thickness = -1.0f;
			recipe->branch_taper = -1.0f;
			recipe->thinnest_branch = -1.0f;
			recipe->tip_lift = -1.0f;
			recipe->collar = -1.0f;
			recipe->root_flare = -1.0f;
			recipe->stems = -1;
			recipe->twig_meters = -1.0f;
			recipe->fork_height = -1.0f;
			recipe->extra_stem_chance = -1.0f;
			recipe->gnarl = -1.0f;
			recipe->kinks = -1.0f;
			recipe->lean_degrees = -1.0f;
			recipe->lean_length = -1.0f;
			recipe->straighten = -1.0f;
			recipe->wind_side = -1.0f;
		}
		nk_tree_pop(context);
	}
	// Through the seasons
	if (nk_tree_push_id(context, NK_TREE_TAB, "Seasons", NK_MINIMIZED, 8))
	{
		float colors[2][3];
		bool falls = recipe->falls >= 0 ? recipe->falls != 0 : plant_kind_falls(kind);
		bool falls_before = falls;

		nk_layout_row_dynamic(context, ROW, 1);
		kind_setting(context, "Moss (north side)", &recipe->moss, plant_kind_moss(kind), 0.0f, 1.0f, 0.05f, 1.0f);
		kind_setting(context, "Light tips (new growth)", &recipe->tips, plant_kind_tips(kind), 0.0f, 2.0f, 0.05f, 1.0f);
		kind_setting(context, "Autumn: how much it turns", &recipe->autumn, plant_kind_autumn(kind), 0.0f, 1.0f, 0.05f, 1.0f);
		falls = nk_check_label(context, "Autumn: the foliage falls", falls);
		if (falls != falls_before)
		{
			recipe->falls = falls ? 1 : 0;
		}
		if (recipe->autumn_colors[0][0] >= 0.0f)
		{
			memcpy(colors, recipe->autumn_colors, sizeof(colors));
		}
		else
		{
			plant_kind_autumn_colors(kind, colors);
		}
		for (int c = 0; c < 2; c++)
		{
			struct nk_colorf color = { colors[c][0], colors[c][1], colors[c][2], 1.0f };
			struct nk_colorf before = color;

			label(context, c == 0 ? "Autumn color, one end" : "Autumn color, the other");
			nk_layout_row_dynamic(context, 90.0f, 1);
			color = nk_color_picker(context, color, NK_RGB);
			nk_layout_row_dynamic(context, ROW, 1);
			if (color.r != before.r || color.g != before.g || color.b != before.b)
			{
				colors[c][0] = color.r;
				colors[c][1] = color.g;
				colors[c][2] = color.b;
				memcpy(recipe->autumn_colors, colors, sizeof(colors));
			}
		}
		// The autumn's months, and the leaves' (season.h)
		{
			int first = recipe->autumn_first_month > 0 ? recipe->autumn_first_month : plant_kind_autumn_first_month(kind);
			int last = recipe->autumn_last_month > 0 ? recipe->autumn_last_month : plant_kind_autumn_last_month(kind);
			int leaves = recipe->leaves_month > 0 ? recipe->leaves_month : plant_kind_leaves_month(kind);
			int first_before = first;
			int last_before = last;
			int leaves_before = leaves;

			nk_property_int(context, "Autumn from month", 1, &first, 12, 1, 0.2f);
			nk_property_int(context, "Autumn to month (bare after)", 1, &last, 12, 1, 0.2f);
			nk_property_int(context, "Leaves again in month", 1, &leaves, 12, 1, 0.2f);
			if (first != first_before || last != last_before)
			{
				recipe->autumn_first_month = first;
				recipe->autumn_last_month = last;
			}
			if (leaves != leaves_before)
			{
				recipe->leaves_month = leaves;
			}
		}
		kind_setting(context, "Snow: sag (% of height)", &recipe->snow_sag, plant_kind_snow_sag(kind), 0.0f, 0.3f, 0.005f, 100.0f);
		if (nk_button_label(context, "The kind's seasons again"))
		{
			recipe->moss = -1.0f;
			recipe->tips = -1.0f;
			recipe->autumn = -1.0f;
			recipe->falls = -1;
			recipe->autumn_colors[0][0] = -1.0f;
			recipe->autumn_first_month = 0;
			recipe->autumn_last_month = 0;
			recipe->leaves_month = 0;
			recipe->snow_sag = -1.0f;
		}
		nk_tree_pop(context);
	}
	// The pictures
	if (nk_tree_push_id(context, NK_TREE_TAB, "Pictures", NK_MAXIMIZED, 4))
	{
		static const char *const bark_names[3] = { "Bark", "  normal", "  arm" };

		for (int k = 0; k < 3; k++)
		{
			char text[PLANT_PATH_LENGTH + 32];

			nk_layout_row_begin(context, NK_DYNAMIC, ROW, 3);
			nk_layout_row_push(context, 0.68f);
			snprintf(text, sizeof(text), "%s: %s", bark_names[k], recipe->bark[k][0] != '\0' ? base_name(recipe->bark[k]) : "-");
			label(context, text);
			nk_layout_row_push(context, 0.18f);
			if (nk_button_label(context, "Pick..."))
			{
				ask_file(editor, PICK_BARK, k, 0, false, false);
			}
			nk_layout_row_push(context, 0.14f);
			if (nk_button_label(context, "x"))
			{
				recipe->bark[k][0] = '\0';
			}
			nk_layout_row_end(context);
		}
		nk_layout_row_dynamic(context, ROW, 1);
		label(context, "  (the bark must be under data/)");
		// Without a bark picture: its plain color
		if (recipe->bark[0][0] == '\0')
		{
			float plain[3];
			struct nk_colorf color;
			struct nk_colorf before;

			plant_stalk_color(recipe, kind, plain);
			color.r = plain[0];
			color.g = plain[1];
			color.b = plain[2];
			color.a = 1.0f;
			before = color;
			nk_layout_row_begin(context, NK_DYNAMIC, ROW, 2);
			nk_layout_row_push(context, 0.68f);
			label(context, recipe->stalk_color[0] >= 0.0f ? "Stalks' color (no bark)" : "Stalks' color (the kind's)");
			nk_layout_row_push(context, 0.32f);
			if (nk_button_label(context, "The kind's"))
			{
				recipe->stalk_color[0] = -1.0f;
			}
			nk_layout_row_end(context);
			nk_layout_row_dynamic(context, 90.0f, 1);
			color = nk_color_picker(context, color, NK_RGB);
			nk_layout_row_dynamic(context, ROW, 1);
			if (color.r != before.r || color.g != before.g || color.b != before.b)
			{
				recipe->stalk_color[0] = color.r;
				recipe->stalk_color[1] = color.g;
				recipe->stalk_color[2] = color.b;
			}
		}
		pictures_section(editor, context, tree ? "Leaves" : (mushroom ? "Texture (as mushroom_template.png; none: the colors')"
			: "Leaves (blades, fronds)"), 0, PICK_LEAVES);
		if (tree)
		{
			pictures_section(editor, context, "Twigs", 1, PICK_TWIGS);
			pictures_section(editor, context, "Fruit", 3, PICK_FRUIT);
		}
		else if (!mushroom)
		{
			pictures_section(editor, context, "Heads (flowers)", 2, PICK_HEADS);
		}
		nk_tree_pop(context);
	}
	// Mushrooms: how many together
	if (mushroom && nk_tree_push_id(context, NK_TREE_TAB, "Mushrooms", NK_MAXIMIZED, 9))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		nk_property_int(context, "Together, at least", 1, &recipe->cluster_minimum, 20, 1, 0.2f);
		nk_property_int(context, "Together, at most", recipe->cluster_minimum, &recipe->cluster_maximum, 20, 1, 0.2f);
		mushroom_section(context, &recipe->mushroom, recipe->leaves.count == 0);
		nk_tree_pop(context);
	}
	// The fruit
	if (tree && nk_tree_push_id(context, NK_TREE_TAB, "Fruit", recipe->fruit.count > 0 ? NK_MAXIMIZED : NK_MINIMIZED, 5))
	{
		static const char *const shapes[] = { "sphere", "spindle", "pod", "card" };
		static const char *const hangs[] = { "the kind's", "down", "up", "out" };
		static const char *const zones[] = { "the kind's", "outer", "top", "all" };
		int hang = recipe->fruit_hang + 1;
		int zone = recipe->fruit_zone + 1;
		int shape = (int)recipe->fruit_shape;

		nk_layout_row_dynamic(context, ROW, 2);
		label(context, "Shape");
		shape = nk_combo(context, shapes, 4, shape, (int)ROW, nk_vec2(200.0f, 200.0f));
		recipe->fruit_shape = (plant_fruit_shape_t)shape;
		label(context, "Hangs");
		hang = nk_combo(context, hangs, 4, hang, (int)ROW, nk_vec2(200.0f, 200.0f));
		recipe->fruit_hang = hang - 1;
		label(context, "Where");
		zone = nk_combo(context, zones, 4, zone, (int)ROW, nk_vec2(200.0f, 200.0f));
		recipe->fruit_zone = zone - 1;
		nk_layout_row_dynamic(context, ROW, 1);
		nk_property_float(context, "Length (m)", 0.002f, &recipe->fruit_length, 2.0f, 0.005f, 0.001f);
		nk_property_float(context, "Width (m, 0: the picture's)", 0.0f, &recipe->fruit_width, 2.0f, 0.005f, 0.001f);
		nk_property_float(context, "Stalk (m)", 0.0f, &recipe->fruit_stalk, 2.0f, 0.005f, 0.001f);
		nk_property_int(context, "Together, at least", 1, &recipe->cluster_minimum, 20, 1, 0.2f);
		nk_property_int(context, "Together, at most", recipe->cluster_minimum, &recipe->cluster_maximum, 20, 1, 0.2f);
		nk_property_float(context, "Clusters per finest branch", 0.0f, &recipe->fruit_per_branch, 50.0f, 0.05f, 0.01f);
		nk_property_float(context, "Bunch (0: fanned from a point)", 0.0f, &recipe->fruit_bunch, 1.0f, 0.05f, 0.01f);
		nk_property_float(context, "Towards the tip", 0.0f, &recipe->fruit_tip, 1.0f, 0.05f, 0.01f);
		nk_tree_pop(context);
	}
	// The view
	if (nk_tree_push_id(context, NK_TREE_TAB, "View", NK_MINIMIZED, 6))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		label(context, "  F12 (or Print Screen): a screenshot, here");
		nk_property_float(context, "Sun, round (degrees)", -360.0f, &editor->sun_azimuth_degrees, 360.0f, 5.0f, 1.0f);
		nk_property_float(context, "Sun, up (degrees)", 1.0f, &editor->sun_elevation_degrees, 90.0f, 2.0f, 0.5f);
		nk_property_float(context, "Wind (0..1)", 0.0f, &editor->wind, 1.0f, 0.05f, 0.01f);
		// Its own wind: written into the object (objects.ini's sway, flutter,
		// stiffness in its type's own section still win)
		if (mushroom)
		{
			label(context, "  Sway: none (a mushroom stands still)");
		}
		else
		{
			float kind_wind[3];
			float grown = editor->mesh_ok ? editor->mesh.height : plant_kind_height(kind);
			float leaf = recipe->leaf_size > 0.0f ? recipe->leaf_size : plant_kind_leaf_size(kind);

			plant_kind_wind(kind, grown, leaf, kind_wind);
			label(context, "  Its wind (in the object; 'the kind's' by its height):");
			kind_setting(context, "Sway: top's lean in a gale (m)", &recipe->sway, kind_wind[0], 0.0f, 5.0f, 0.01f, 1.0f);
			kind_setting(context, "Flutter: leaf tips shake (m)", &recipe->flutter, kind_wind[1], 0.0f, 0.5f, 0.002f, 1.0f);
			kind_setting(context, "Stiffness: still up to wind", &recipe->stiffness, kind_wind[2], 0.0f, 0.95f, 0.01f, 1.0f);
			if (nk_button_label(context, "The kind's wind"))
			{
				recipe->sway = -1.0f;
				recipe->flutter = -1.0f;
				recipe->stiffness = -1.0f;
			}
		}
		nk_property_float(context, "Sky light (ambient)", 0.0f, &editor->ambient, 2.0f, 0.05f, 0.01f);
		nk_property_float(context, "Exposure (stops)", -6.0f, &editor->settings.render.exposure_ev, 6.0f, 0.25f, 0.05f);
		// The month, as the game's render.month, and the snow's months
		{
			char month_label[48];

			SDL_snprintf(month_label, sizeof(month_label), "Month (%s)", season_month_name(editor->month));
			nk_property_float(context, month_label, 1.0f, &editor->month, 12.99f, 0.25f, 0.02f);
			nk_property_float(context, "Snow from month", 1.0f, &editor->settings.render.snow_first_month, 12.0f, 1.0f, 0.2f);
			nk_property_float(context, "Snow to month", 1.0f, &editor->settings.render.snow_last_month, 12.0f, 1.0f, 0.2f);
			nk_property_float(context, "New growth from month", 1.0f, &editor->settings.render.tips_first_month, 12.0f, 1.0f, 0.2f);
			nk_property_float(context, "New growth to month", 1.0f, &editor->settings.render.tips_last_month, 12.0f, 1.0f, 0.2f);
		}
		// The levels of detail the game will draw it at, farther away
		nk_property_int(context, "Detail level (0: all)", 0, &editor->detail, 3, 1, 0.2f);
		{
			float kept = editor->foliage_kept * 100.0f;

			nk_property_float(context, "Foliage kept (%)", 5.0f, &kept, 100.0f, 5.0f, 1.0f);
			editor->foliage_kept = kept / 100.0f;
		}
		if (editor->mesh_ok && editor->mesh.lods[PLANT_MATERIAL_BARK])
		{
			char text[128];

			snprintf(text, sizeof(text), "  branches: %zu, %u, %u, %u triangles", editor->mesh.index_count[PLANT_MATERIAL_BARK] / 3,
				editor->mesh.lod_index_count[PLANT_MATERIAL_BARK][0] / 3, editor->mesh.lod_index_count[PLANT_MATERIAL_BARK][1] / 3,
				editor->mesh.lod_index_count[PLANT_MATERIAL_BARK][2] / 3);
			label(context, text);
		}
		nk_tree_pop(context);
	}
	// Variants
	if (nk_tree_push_id(context, NK_TREE_TAB, "Variants", NK_MINIMIZED, 7))
	{
		nk_layout_row_dynamic(context, ROW, 1);
		label(context, "The next seeds too, as name_1.pobj, name_2.pobj...");
		nk_layout_row_dynamic(context, ROW, 2);
		nk_property_int(context, "How many", 1, &editor->variants, 50, 1, 0.2f);
		if (nk_button_label(context, "Save them"))
		{
			save(editor, editor->variants);
		}
		nk_tree_pop(context);
	}
	// What there is
	{
		char text[256];

		nk_layout_row_dynamic(context, ROW, 1);
		if (editor->mesh_ok)
		{
			size_t triangles = 0;

			for (int m = 0; m < PLANT_MATERIAL_COUNT; m++)
			{
				triangles += editor->mesh.index_count[m] / 3;
			}
			snprintf(text, sizeof(text), "%zu triangles (%zu foliage, %zu fruit), %d fruit", triangles,
				editor->mesh.index_count[PLANT_MATERIAL_FOLIAGE] / 3, editor->mesh.index_count[PLANT_MATERIAL_FRUIT] / 3, editor->mesh.fruit_count);
			label(context, text);
		}
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------

static void view_of(const editor_t *editor, int width, int height, plant_view_t *view)
{
	float yaw = editor->yaw_degrees * SDL_PI_F / 180.0f;
	float pitch = editor->pitch_degrees * SDL_PI_F / 180.0f;

	SDL_zerop(view);
	// Over the plant's middle (a leaning one's crown is not over its root)
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
	view->wind = editor->wind;
	// Its own wind, as the object will carry it (a mushroom stands still in
	// any wind)
	{
		int kind = plant_kind_find(editor->recipe.kind);
		float plant_wind[3] = { 0.0f, 0.0f, 0.0f };

		if (kind >= 0 && !plant_kind_is_mushroom(kind))
		{
			plant_recipe_wind(&editor->recipe, kind, editor->mesh_ok ? editor->mesh.height : plant_kind_height(kind),
				editor->recipe.leaf_size > 0.0f ? editor->recipe.leaf_size : plant_kind_leaf_size(kind), plant_wind);
		}
		view->sway = plant_wind[0];
		view->flutter = plant_wind[1];
		view->stiffness = plant_wind[2];
	}
	view->ambient = editor->ambient;
	view->detail = editor->detail;
	view->month = editor->month;
	view->snow_first_month = SDL_roundf(editor->settings.render.snow_first_month);
	view->snow_last_month = SDL_roundf(editor->settings.render.snow_last_month);
	view->tips_first_month = SDL_roundf(editor->settings.render.tips_first_month);
	view->tips_last_month = SDL_roundf(editor->settings.render.tips_last_month);
	view->foliage_kept = editor->foliage_kept;
	view->time_seconds = (float)((double)SDL_GetTicks() / 1000.0);
}

// The view fits the plant once it first grows (and after the kind changes)
static void frame_plant(editor_t *editor)
{
	float height = editor->mesh_ok ? editor->mesh.maximum[1] - editor->mesh.minimum[1] : 2.0f;
	float width = editor->mesh_ok ? SDL_max(editor->mesh.maximum[0] - editor->mesh.minimum[0], editor->mesh.maximum[2] - editor->mesh.minimum[2])
		: 2.0f;

	editor->look_height = height * 0.45f;
	// (as near as the wheel goes: a mushroom is a hand high)
	editor->distance = SDL_max(SDL_max(height, width * 0.8f) * 2.1f, 0.3f);
}

int main(int argument_count, char **arguments)
{
	static editor_t editor;
	const char *screenshot_path = NULL;
	int screenshot_frames = 30;
	bool shoot_now = false;
	float fixed_distance = 0.0f;              // --distance (the tests'): instead of framing the plant
	int frame_number = 0;
	bool running = true;
	bool save_first = false;
	char framed_kind[32] = "";

	editor.data_directory = "data";
	plant_recipe_defaults(&editor.recipe);
	editor.yaw_degrees = 30.0f;
	editor.pitch_degrees = 12.0f;
	editor.distance = 20.0f;
	editor.look_height = 4.0f;
	editor.sun_azimuth_degrees = 60.0f;
	editor.sun_elevation_degrees = 38.0f;
	editor.wind = 0.3f;
	editor.ambient = 0.35f;
	editor.foliage_kept = 1.0f;
	editor.month = 7.5f;
	editor.months_list = -1;
	editor.months_index = -1;
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
		else if (strcmp(arguments[i], "--month") == 0 && i + 1 < argument_count)
		{
			float month = (float)SDL_atof(arguments[++i]);

			editor.month = SDL_clamp(month, 1.0f, 12.99f);
		}
		else if (strcmp(arguments[i], "--detail") == 0 && i + 1 < argument_count)
		{
			// (SDL_clamp is a macro: its argument once, in a variable)
			int detail = SDL_atoi(arguments[++i]);

			editor.detail = SDL_clamp(detail, 0, 3);
		}
		else if (strcmp(arguments[i], "--foliage-kept") == 0 && i + 1 < argument_count)
		{
			float kept = (float)SDL_atof(arguments[++i]);

			editor.foliage_kept = SDL_clamp(kept, 0.05f, 1.0f);
		}
		else if (strcmp(arguments[i], "--wind") == 0 && i + 1 < argument_count)
		{
			float wind = (float)SDL_atof(arguments[++i]);

			editor.wind = SDL_clamp(wind, 0.0f, 1.0f);
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
	editor.window = SDL_CreateWindow("Potop plant editor", 1500, 950, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (editor.window == NULL || !gpu_create(editor.window, screenshot_path == NULL, 8.0f))
	{
		SDL_Log("No window or no GPU: %s", SDL_GetError());
		return 1;
	}
	settings_set_defaults(&editor.settings);
	// A fixed exposure: the plants compared under the same light, whatever
	// fills the view (the automatic one followed the bright sky)
	editor.settings.render.auto_exposure = false;
	editor.settings.render.exposure_ev = EDITOR_EXPOSURE_EV;
	if (!post_create(&editor.post) || !plant_preview_create(&editor.preview)
		|| !ui_backend_create(&editor.ui, editor.window, "ui/fonts/DejaVuSans.ttf", FONT_SIZE, 1.0f, 1.0f))
	{
		SDL_Log("The editor cannot start (shaders, font?): %s", SDL_GetError());
		return 1;
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
			// F12 (or Print Screen): the window as it is, as the game's
			// (screenshot_<date>_<time>.png where the editor runs)
			if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
				&& (event.key.key == SDLK_F12 || event.key.key == SDLK_PRINTSCREEN))
			{
				shoot_now = true;
			}
			if (interface)
			{
				continue;
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			{
				// The wheel's button turns as the left one does
				editor.turning = editor.turning || event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_MIDDLE;
				editor.raising = editor.raising || event.button.button == SDL_BUTTON_RIGHT;
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
			{
				// Still turning while the other of the two is held
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
				editor.distance = SDL_clamp(editor.distance * (event.wheel.y > 0.0f ? 0.9f : 1.1f), 0.3f, 300.0f);
			}
		}
		ui_backend_input_end(&editor.ui);
		take_answer(&editor);
		panel(&editor);
		ui_backend_update_capture(&editor.ui);
		grow_if_changed(&editor);
		if (save_first)
		{
			save_first = false;
			save(&editor, 0);
		}
		if (strcmp(framed_kind, editor.recipe.kind) != 0 && editor.mesh_ok)
		{
			snprintf(framed_kind, sizeof(framed_kind), "%s", editor.recipe.kind);
			frame_plant(&editor);
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
			plant_view_t view;

			plant_preview_sky(sky);
			pass = render_target_begin(&editor.target, pixel_width, pixel_height, 1.0f, sky, &scene_width, &scene_height);
			if (pass != NULL)
			{
				// The plant in the part of the window right of the panel
				float panel_pixels = PANEL_WIDTH * editor.ui.scale * (float)pixel_width / (float)SDL_max(window_width, 1);
				SDL_GPUViewport viewport = { panel_pixels, 0.0f, (float)scene_width - panel_pixels, (float)scene_height, 0.0f, 1.0f };

				SDL_SetGPUViewport(pass, &viewport);
				view_of(&editor, (int)viewport.w, scene_height, &view);
				plant_preview_draw(&editor.preview, pass, &view);
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
	plant_mesh_free(&editor.mesh);
	plant_atlas_free(editor.atlas);
	SDL_free(editor.atlas_signature);
	ui_backend_destroy(&editor.ui);
	plant_preview_destroy(&editor.preview);
	post_destroy(&editor.post);
	render_target_destroy(&editor.target);
	gpu_destroy();
	SDL_DestroyMutex(editor.pick_lock);
	SDL_DestroyWindow(editor.window);
	SDL_Quit();
	return 0;
}
