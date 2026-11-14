// potop_prop: makes a prop (a rock, a stone wall, a well, a wooden fence)
// into an object for the game (.pobj), from the command line: see
// tools/prop_grow.h and prop_grow.c for how. A desktop tool: it is not part
// of the game and never ships with it. The same in a window, with the prop
// in view while it changes: tools/prop_editor.
//
//   potop_prop [options] [data/objects/name.pobj]
//
// Options:
//   --data DIR                 the game's data directory (data): the textures are named from it
//   --recipe FILE              a recipe (prop_plans/): read first, the options after it change it
//   --preset NAME              start from a preset: boulder, river_stone, slab, crag, holed_rock, split_rock,
//                              pebbles, dry_wall, ruined_wall, well, rail_fence, picket_fence, wine_barrel, water_barrel,
//                              lying_barrel, broken_barrel, tub, bucket
//   --seed N                   which one (1)
//   --set KEY=VALUE            any of the recipe's settings (as in a recipe: length=2.5, moss=0.8...)
//   --save-recipe FILE         writes the recipe the options make (and the prop too)
//   --variants N               the next N seeds too, as name_1.pobj .. name_N.pobj (kept in a recipe it saves)
//   --all DIR                  (after --data) makes every recipe in DIR again (DIR/*.prop: each its output and its variants)
//                              and nothing else: after the tool changes, the props as it makes them now
//   --textures STYLE           (re)makes the style's stand-in textures and does nothing else:
//                              granite, sandstone, slate, limestone, wood, old_wood
//   --wood-template FILE       writes the wood's texture layout (a picture to paint over) and nothing else
//   --list                     the presets and the settings
//
// The object goes where the options say, else the recipe's output, else
// data/objects/<preset>.pobj.

// (opendir: POSIX)
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prop_grow.h"

static void usage(void)
{
	fprintf(stderr, "potop_prop [--data DIR] [--recipe FILE] [--preset NAME] [--seed N] [--set KEY=VALUE]... [--save-recipe FILE]\n"
		"           [--variants N] [OUTPUT.pobj]\n"
		"potop_prop --textures STYLE | --wood-template FILE | --list\n");
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

static bool make_one(const prop_recipe_t *recipe, const char *data_directory, const char *path)
{
	prop_mesh_t mesh;

	if (!prop_grow(recipe, data_directory, &mesh))
	{
		fprintf(stderr, "potop_prop: %s\n", prop_last_error());
		return false;
	}
	if (!prop_write(path, &mesh))
	{
		fprintf(stderr, "potop_prop: %s\n", prop_last_error());
		prop_mesh_free(&mesh);
		return false;
	}
	printf("%s: %zu triangles, %.2f x %.2f x %.2f m", path, prop_mesh_triangles(&mesh, 0),
		(double)(mesh.maximum[0] - mesh.minimum[0]), (double)(mesh.maximum[1] - mesh.minimum[1]), (double)(mesh.maximum[2] - mesh.minimum[2]));
	if (prop_mesh_has_levels(&mesh))
	{
		printf("; farther %zu, %zu, %zu (off by %.1f, %.1f, %.1f mm)", prop_mesh_triangles(&mesh, 1), prop_mesh_triangles(&mesh, 2),
			prop_mesh_triangles(&mesh, 3), (double)mesh.lod_errors[0] * 1000.0, (double)mesh.lod_errors[1] * 1000.0, (double)mesh.lod_errors[2] * 1000.0);
	}
	printf("\n");
	prop_mesh_free(&mesh);
	return true;
}

// A recipe's object and its variants
static bool make_all_of(const prop_recipe_t *recipe, const char *data_directory, const char *output)
{
	if (!make_one(recipe, data_directory, output))
	{
		return false;
	}
	for (int v = 1; v <= recipe->variants; v++)
	{
		prop_recipe_t variant = *recipe;
		char path[PROP_PATH_LENGTH + 32];

		variant.seed = recipe->seed + (uint32_t)v;
		variant_path(output, v, path, sizeof(path));
		if (!make_one(&variant, data_directory, path))
		{
			return false;
		}
	}
	return true;
}

// --all: every recipe in a folder again
static int make_folder(const char *folder, const char *data_directory)
{
	DIR *directory = opendir(folder);
	struct dirent *entry;
	int made = 0;
	int failed = 0;

	if (directory == NULL)
	{
		fprintf(stderr, "potop_prop: cannot read the folder '%s'\n", folder);
		return 1;
	}
	while ((entry = readdir(directory)) != NULL)
	{
		size_t length = strlen(entry->d_name);
		char path[PROP_PATH_LENGTH * 2];
		char output[PROP_PATH_LENGTH + 64];
		prop_recipe_t recipe;

		if (length < 6 || strcmp(entry->d_name + length - 5, ".prop") != 0)
		{
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (!prop_recipe_load(&recipe, path))
		{
			fprintf(stderr, "potop_prop: %s\n", prop_last_error());
			failed++;
			continue;
		}
		if (recipe.output[0] != '\0')
		{
			snprintf(output, sizeof(output), "%s", recipe.output);
		}
		else
		{
			snprintf(output, sizeof(output), "%s/objects/%.*s.pobj", data_directory, (int)(length - 5), entry->d_name);
		}
		if (make_all_of(&recipe, data_directory, output))
		{
			made++;
		}
		else
		{
			failed++;
		}
	}
	closedir(directory);
	printf("%d recipes made again%s\n", made, failed > 0 ? ", some failed (see above)" : "");
	return failed > 0 ? 1 : 0;
}

int main(int argument_count, char **arguments)
{
	prop_recipe_t recipe;
	const char *data_directory = "data";
	const char *output = NULL;
	const char *recipe_out = NULL;

	prop_recipe_defaults(&recipe);
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		if (strcmp(argument, "--data") == 0 && value != NULL)
		{
			data_directory = value;
			i++;
		}
		else if (strcmp(argument, "--recipe") == 0 && value != NULL)
		{
			if (!prop_recipe_load(&recipe, value))
			{
				fprintf(stderr, "potop_prop: %s\n", prop_last_error());
				return 1;
			}
			i++;
		}
		else if (strcmp(argument, "--preset") == 0 && value != NULL)
		{
			if (!prop_preset(&recipe, value))
			{
				fprintf(stderr, "potop_prop: %s\n", prop_last_error());
				return 1;
			}
			i++;
		}
		else if (strcmp(argument, "--seed") == 0 && value != NULL)
		{
			recipe.seed = (uint32_t)strtoul(value, NULL, 10);
			i++;
		}
		else if (strcmp(argument, "--set") == 0 && value != NULL)
		{
			char setting[PROP_PATH_LENGTH + 64];
			char *equals;

			snprintf(setting, sizeof(setting), "%s", value);
			equals = strchr(setting, '=');
			if (equals == NULL)
			{
				fprintf(stderr, "potop_prop: --set wants KEY=VALUE, not '%s'\n", value);
				return 1;
			}
			*equals = '\0';
			if (!prop_recipe_set(&recipe, setting, equals + 1))
			{
				fprintf(stderr, "potop_prop: %s\n", prop_last_error());
				return 1;
			}
			i++;
		}
		else if (strcmp(argument, "--save-recipe") == 0 && value != NULL)
		{
			recipe_out = value;
			i++;
		}
		else if (strcmp(argument, "--variants") == 0 && value != NULL)
		{
			recipe.variants = atoi(value);
			i++;
		}
		else if (strcmp(argument, "--all") == 0 && value != NULL)
		{
			return make_folder(value, data_directory);
		}
		else if (strcmp(argument, "--textures") == 0 && value != NULL)
		{
			char names[3][PROP_PATH_LENGTH];

			if (!prop_style_textures(value, data_directory, true, names))
			{
				fprintf(stderr, "potop_prop: %s\n", prop_last_error());
				return 1;
			}
			printf("%s/%s\n%s/%s\n%s/%s\n", data_directory, names[0], data_directory, names[1], data_directory, names[2]);
			return 0;
		}
		else if (strcmp(argument, "--wood-template") == 0 && value != NULL)
		{
			if (!prop_wood_template(value))
			{
				fprintf(stderr, "potop_prop: %s\n", prop_last_error());
				return 1;
			}
			printf("%s\n", value);
			return 0;
		}
		else if (strcmp(argument, "--list") == 0)
		{
			printf("Presets:");
			for (int p = 0; p < prop_preset_count(); p++)
			{
				printf(" %s", prop_preset_name(p));
			}
			printf("\nSettings: as in a recipe (prop_recipe_save writes them all):\n");
			prop_recipe_save(&recipe, "/dev/stdout");
			return 0;
		}
		else if (argument[0] != '-')
		{
			output = argument;
		}
		else
		{
			usage();
			return 1;
		}
	}
	if (output == NULL)
	{
		static char named[PROP_PATH_LENGTH + 64];

		if (recipe.output[0] != '\0')
		{
			output = recipe.output;
		}
		else
		{
			snprintf(named, sizeof(named), "%s/objects/%s.pobj", data_directory, recipe.preset[0] != '\0' ? recipe.preset : "prop");
			output = named;
		}
	}
	if (recipe_out != NULL)
	{
		snprintf(recipe.output, sizeof(recipe.output), "%s", output);
		if (!prop_recipe_save(&recipe, recipe_out))
		{
			fprintf(stderr, "potop_prop: %s\n", prop_last_error());
			return 1;
		}
	}
	// (its variants: --variants, or the recipe's)
	if (!make_all_of(&recipe, data_directory, output))
	{
		return 1;
	}
	return 0;
}
