// potop_house: houses made from nothing, straight into the game's objects
// (.pobj). See house_grow.h.
//
//   tools/potop_house [--recipe FILE | --preset NAME] [--seed N] [--set KEY=VALUE]...
//                     [--save-recipe FILE] [--variants N] [--format V] [--sections FILE] [--data DIR] [OUTPUT.pobj]
//   tools/potop_house --all DIR            every recipe in DIR (house_plans/), variants and all
//   tools/potop_house --list               the presets and every setting
//   tools/potop_house --textures           the stand-in textures made again
//
// With no output named, the house goes to data/objects/<preset>.pobj (or
// the recipe's output). --sections prints the objects.ini and layouts.ini
// sections for it.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "house_grow.h"

// The recipe being made (its directory: where its module's recipes are)
static const char *recipe_path_in_use = NULL;

static bool grow_and_write(const house_recipe_t *recipe, const char *data_directory, const char *output, const char *sections_path);

// The name of a file without its directory and extension
static void stem_of(const char *path, char *out, size_t size)
{
	const char *slash = strrchr(path, '/');
	char *dot;

	snprintf(out, size, "%s", slash != NULL ? slash + 1 : path);
	dot = strrchr(out, '.');
	if (dot != NULL)
	{
		*dot = '\0';
	}
}

// A module part's recipe (house_module_t.part_recipes): "@N" the Nth of
// the body's module_recipes (next to its recipe), else the body's own; then
// the settings after the ";"s
static bool part_recipe(const house_recipe_t *body, const char *how, house_recipe_t *out)
{
	char text[HOUSE_PATH_LENGTH];
	char *setting;
	char *next;

	snprintf(text, sizeof(text), "%s", how);
	*out = *body;
	setting = text;
	next = strchr(setting, ';');
	if (next != NULL)
	{
		*next++ = '\0';
	}
	if (setting[0] == '@')
	{
		int wanted = atoi(setting + 1);
		char list[HOUSE_PATH_LENGTH * 2];
		char *item;
		char *rest;
		int n = 0;
		bool found = false;

		snprintf(list, sizeof(list), "%s", body->module_recipes);
		for (item = list; item != NULL; item = rest)
		{
			rest = strchr(item, ',');
			if (rest != NULL)
			{
				*rest++ = '\0';
			}
			while (*item == ' ')
			{
				item++;
			}
			if (++n == wanted && item[0] != '\0')
			{
				char path[HOUSE_PATH_LENGTH * 2];
				const char *slash = recipe_path_in_use != NULL ? strrchr(recipe_path_in_use, '/') : NULL;

				if (item[0] != '/' && slash != NULL)
				{
					snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - recipe_path_in_use), recipe_path_in_use, item);
				}
				else
				{
					snprintf(path, sizeof(path), "%s", item);
				}
				if (!house_recipe_load(out, path))
				{
					fprintf(stderr, "potop_house: %s\n", house_last_error());
					return false;
				}
				found = true;
				break;
			}
		}
		if (!found)
		{
			fprintf(stderr, "potop_house: no module_recipes item %d: the part grown from the body's recipe\n", wanted);
		}
		setting = next;
		next = setting != NULL ? strchr(setting, ';') : NULL;
		if (next != NULL)
		{
			*next++ = '\0';
		}
	}
	while (setting != NULL)
	{
		char *equals = strchr(setting, '=');

		if (equals != NULL)
		{
			*equals = '\0';
			if (!house_recipe_set(out, setting, equals + 1))
			{
				fprintf(stderr, "potop_house: %s\n", house_last_error());
				return false;
			}
		}
		setting = next;
		next = setting != NULL ? strchr(setting, ';') : NULL;
		if (next != NULL)
		{
			*next++ = '\0';
		}
	}
	out->module[0] = '\0';
	out->module_recipes[0] = '\0';
	out->variants = 0;
	out->seed = body->seed;
	return true;
}

// The module the house makes: its parts grown and written
// (data/objects/MODULE_PART.pobj), then the module (data/modules/MODULE.pmod)
static bool write_module(const house_recipe_t *recipe, const char *data_directory, const char *output, const house_mesh_t *mesh)
{
	house_module_t module = *mesh->module;
	char body_model[MODULE_FILE_MODEL_LENGTH];
	char path[HOUSE_PATH_LENGTH * 2];

	stem_of(output, body_model, sizeof(body_model));
	snprintf(module.file.parts[0].model, sizeof(module.file.parts[0].model), "%s", body_model);
	for (int p = 1; p < module.file.part_count; p++)
	{
		house_recipe_t part;
		char part_output[HOUSE_PATH_LENGTH];
		int same = -1;

		// (grown the same as one before it, a rope's lengths: its model)
		for (int q = 1; q < p && same < 0; q++)
		{
			if (strcmp(module.part_recipes[q], module.part_recipes[p]) == 0)
			{
				same = q;
			}
		}
		if (same >= 0)
		{
			memcpy(module.file.parts[p].model, module.file.parts[same].model, sizeof(module.file.parts[p].model));
			continue;
		}
		if (!part_recipe(recipe, module.part_recipes[p], &part))
		{
			return false;
		}
		snprintf(module.file.parts[p].model, sizeof(module.file.parts[p].model), "%s_%s", recipe->module, module.file.parts[p].name);
		snprintf(part_output, sizeof(part_output), "objects/%s.pobj", module.file.parts[p].model);
		if (!grow_and_write(&part, data_directory, part_output, NULL))
		{
			return false;
		}
	}
	if (module.file.state_count == 0)
	{
		module_file_add_state(&module.file, "standing", 0.0f);
	}
	snprintf(path, sizeof(path), "%s/modules", data_directory);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/modules/%s.pmod", data_directory, recipe->module);
	if (!module_file_write(path, &module.file))
	{
		fprintf(stderr, "potop_house: cannot write '%s'\n", path);
		return false;
	}
	printf("%s: a module of %d parts, %d states\n", path, module.file.part_count, module.file.state_count);
	module_file_dump(&module.file, stdout);
	printf("objects.ini:\n[%s]\nmodule = %s\ncategory = buildings\nfootprint_meters = %.3g\n", body_model, recipe->module,
		(double)mesh->footprint_meters);
	return true;
}

static void usage(void)
{
	printf("potop_house: a house into a .pobj\n\n");
	printf("  --recipe FILE        start from a recipe (house_plans/*.house)\n");
	printf("  --preset NAME        start from a preset (--list shows them)\n");
	printf("  --seed N             its seed\n");
	printf("  --set KEY=VALUE      one setting (--list shows the keys)\n");
	printf("  --save-recipe FILE   write the recipe as it ends up\n");
	printf("  --variants N         make N more with the next seeds, as name_1.pobj...\n");
	printf("  --format V           the .pobj version: 7 with the weathering baked per vertex (default), 6 for an older engine\n");
	printf("  --atlas DIR          write the details atlas' template (house_details_template.png) and the painter's guide\n");
	printf("                       (house_details_atlas.md) into DIR, and remake the stand-in atlas textures\n");
	printf("  --sections FILE      write objects.ini and layouts.ini sections for it (- : stdout)\n");
	printf("  --data DIR           the data directory (data)\n");
	printf("  --all DIR            every recipe in DIR, with its variants\n");
	printf("  --textures           the stand-in textures made again\n");
	printf("  --list               the presets and the settings\n");
}

static bool grow_and_write(const house_recipe_t *recipe, const char *data_directory, const char *output, const char *sections_path)
{
	house_mesh_t mesh;
	char path[HOUSE_PATH_LENGTH * 2];

	if (!house_grow(recipe, data_directory, &mesh))
	{
		fprintf(stderr, "potop_house: %s\n", house_last_error());
		return false;
	}
	if (output[0] == '/')
	{
		snprintf(path, sizeof(path), "%s", output);
	}
	else
	{
		snprintf(path, sizeof(path), "%s/%s", data_directory, output);
	}
	if (!house_write(path, &mesh))
	{
		fprintf(stderr, "potop_house: %s\n", house_last_error());
		house_mesh_free(&mesh);
		return false;
	}
	printf("%s: %zu vertices, %zu triangles (levels: %zu, %zu, %zu; off by %.3f, %.3f, %.3f m), %d anchors, %d windows lit by the game\n", path,
		mesh.vertex_count, house_mesh_triangles(&mesh, 0), house_mesh_triangles(&mesh, 1), house_mesh_triangles(&mesh, 2), house_mesh_triangles(&mesh, 3),
		mesh.lod_errors[0], mesh.lod_errors[1], mesh.lod_errors[2], mesh.anchor_count, mesh.emissive_group_count);
	for (int m = 0; m < mesh.material_count; m++)
	{
		const house_material_t *material = &mesh.materials[m];

		if (material->own_index_count > 0)
		{
			printf("  %-10s %6zu triangles (levels: %zu, %zu, %zu)\n", material->name, material->own_index_count / 3, material->lod_index_count[0] / 3,
				material->lod_index_count[1] / 3, material->lod_index_count[2] / 3);
		}
	}
	if (mesh.module != NULL && !write_module(recipe, data_directory, output, &mesh))
	{
		house_mesh_free(&mesh);
		return false;
	}
	if (sections_path != NULL)
	{
		FILE *file = strcmp(sections_path, "-") == 0 ? stdout : fopen(sections_path, "w");
		char object_name[HOUSE_PATH_LENGTH];
		const char *slash = strrchr(output, '/');
		char *dot;

		snprintf(object_name, sizeof(object_name), "%s", slash != NULL ? slash + 1 : output);
		dot = strrchr(object_name, '.');
		if (dot != NULL)
		{
			*dot = '\0';
		}
		if (file == NULL)
		{
			fprintf(stderr, "potop_house: cannot write '%s'\n", sections_path);
		}
		else
		{
			house_write_sections(file, object_name, &mesh);
			if (file != stdout)
			{
				fclose(file);
			}
		}
	}
	house_mesh_free(&mesh);
	return true;
}

// The output's name with _N before the extension
static void variant_name(const char *output, int n, char *out, size_t size)
{
	const char *dot = strrchr(output, '.');
	const char *slash = strrchr(output, '/');

	if (dot == NULL || (slash != NULL && dot < slash))
	{
		snprintf(out, size, "%s_%d", output, n);
	}
	else
	{
		snprintf(out, size, "%.*s_%d%s", (int)(dot - output), output, n, dot);
	}
}

static bool make_all(const house_recipe_t *recipe, const char *data_directory, const char *output, const char *sections_path)
{
	if (!grow_and_write(recipe, data_directory, output, sections_path))
	{
		return false;
	}
	for (int n = 1; n <= recipe->variants; n++)
	{
		house_recipe_t variant = *recipe;
		char name[HOUSE_PATH_LENGTH];

		variant.seed = recipe->seed + (uint32_t)n;
		variant.module[0] = '\0';
		variant_name(output, n, name, sizeof(name));
		if (!grow_and_write(&variant, data_directory, name, NULL))
		{
			return false;
		}
	}
	return true;
}

static void default_output(const house_recipe_t *recipe, char *out, size_t size)
{
	if (recipe->output[0] != '\0')
	{
		snprintf(out, size, "%s", recipe->output);
	}
	else
	{
		snprintf(out, size, "objects/%s.pobj", recipe->preset);
	}
}

int main(int argument_count, char **arguments)
{
	house_recipe_t recipe;
	const char *data_directory = "data";
	const char *output = NULL;
	const char *save_recipe = NULL;
	const char *sections_path = NULL;
	const char *all_directory = NULL;
	bool list = false;
	bool textures = false;
	const char *atlas_directory = NULL;
	char chosen_output[HOUSE_PATH_LENGTH];

	house_recipe_defaults(&recipe);
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		bool has_next = i + 1 < argument_count;

		if (strcmp(argument, "--recipe") == 0 && has_next)
		{
			recipe_path_in_use = arguments[i + 1];
			if (!house_recipe_load(&recipe, arguments[++i]))
			{
				fprintf(stderr, "potop_house: %s\n", house_last_error());
				return 1;
			}
		}
		else if (strcmp(argument, "--preset") == 0 && has_next)
		{
			if (!house_preset(&recipe, arguments[++i]))
			{
				fprintf(stderr, "potop_house: %s\n", house_last_error());
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
			if (equals == NULL)
			{
				fprintf(stderr, "potop_house: --set wants KEY=VALUE\n");
				return 1;
			}
			*equals = '\0';
			if (!house_recipe_set(&recipe, pair, equals + 1))
			{
				fprintf(stderr, "potop_house: %s\n", house_last_error());
				return 1;
			}
		}
		else if (strcmp(argument, "--save-recipe") == 0 && has_next)
		{
			save_recipe = arguments[++i];
		}
		else if (strcmp(argument, "--variants") == 0 && has_next)
		{
			recipe.variants = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--atlas") == 0 && has_next)
		{
			atlas_directory = arguments[++i];
		}
		else if (strcmp(argument, "--format") == 0 && has_next)
		{
			recipe.format = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--sections") == 0 && has_next)
		{
			sections_path = arguments[++i];
		}
		else if (strcmp(argument, "--data") == 0 && has_next)
		{
			data_directory = arguments[++i];
		}
		else if (strcmp(argument, "--all") == 0 && has_next)
		{
			all_directory = arguments[++i];
		}
		else if (strcmp(argument, "--textures") == 0)
		{
			textures = true;
		}
		else if (strcmp(argument, "--list") == 0)
		{
			list = true;
		}
		else if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0)
		{
			usage();
			return 0;
		}
		else if (argument[0] == '-')
		{
			fprintf(stderr, "potop_house: unknown option '%s'\n", argument);
			usage();
			return 1;
		}
		else
		{
			output = argument;
		}
	}
	if (list)
	{
		printf("Presets:");
		for (int p = 0; p < house_preset_count(); p++)
		{
			printf(" %s", house_preset_name(p));
		}
		printf("\n\nSettings (with the current recipe's values):\n");
		house_recipe_print(&recipe);
		return 0;
	}
	if (atlas_directory != NULL)
	{
		char png[HOUSE_PATH_LENGTH * 2];
		char guide[HOUSE_PATH_LENGTH * 2];
		char made[4][HOUSE_PATH_LENGTH];

		snprintf(png, sizeof(png), "%s/house_details_template.png", atlas_directory);
		snprintf(guide, sizeof(guide), "%s/house_details_atlas.md", atlas_directory);
		if (!house_atlas_template(png, guide))
		{
			fprintf(stderr, "potop_house: cannot write the atlas template into '%s'\n", atlas_directory);
			return 1;
		}
		if (!house_atlas_stand_in(data_directory, true, made))
		{
			fprintf(stderr, "potop_house: cannot make the stand-in atlas\n");
			return 1;
		}
		printf("%s\n%s\n%s, %s, %s, %s\n", png, guide, made[0], made[1], made[2], made[3]);
		return 0;
	}
	if (textures)
	{
		static const char *const names[] = { "plaster", "timber", "roof_tile", "base_stone", "wall_stone", "thatch", "daub", "rubble",
			"board_wall" };
		int count = (int)(sizeof(names) / sizeof(names[0]));

		// Every one, and the plaster in each of its tints
		for (int i = 0; i < count + house_tint_count(); i++)
		{
			char made[4][HOUSE_PATH_LENGTH];
			char name[64];

			if (i < count)
			{
				snprintf(name, sizeof(name), "%s", names[i]);
			}
			else
			{
				snprintf(name, sizeof(name), "plaster_%s", house_tint_name(i - count));
			}
			if (!house_stand_in_textures(name, data_directory, true, made))
			{
				fprintf(stderr, "potop_house: cannot make the %s textures\n", name);
				return 1;
			}
			printf("%s: %s, %s, %s\n", name, made[0], made[1], made[2]);
		}
		return 0;
	}
	if (all_directory != NULL)
	{
		DIR *directory = opendir(all_directory);
		struct dirent *entry;
		bool ok = true;

		if (directory == NULL)
		{
			fprintf(stderr, "potop_house: cannot read '%s'\n", all_directory);
			return 1;
		}
		while ((entry = readdir(directory)) != NULL)
		{
			size_t length = strlen(entry->d_name);
			char path[HOUSE_PATH_LENGTH * 2];
			house_recipe_t one;

			if (length < 7 || strcmp(entry->d_name + length - 6, ".house") != 0)
			{
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s", all_directory, entry->d_name);
			recipe_path_in_use = path;
			if (!house_recipe_load(&one, path))
			{
				fprintf(stderr, "potop_house: %s\n", house_last_error());
				ok = false;
				continue;
			}
			if (one.output[0] == '\0')
			{
				snprintf(one.output, sizeof(one.output), "objects/%.*s.pobj", (int)(length - 6), entry->d_name);
			}
			ok = make_all(&one, data_directory, one.output, NULL) && ok;
		}
		closedir(directory);
		return ok ? 0 : 1;
	}
	if (save_recipe != NULL && !house_recipe_save(&recipe, save_recipe))
	{
		fprintf(stderr, "potop_house: %s\n", house_last_error());
		return 1;
	}
	if (output == NULL)
	{
		default_output(&recipe, chosen_output, sizeof(chosen_output));
		output = chosen_output;
	}
	return make_all(&recipe, data_directory, output, sections_path) ? 0 : 1;
}
