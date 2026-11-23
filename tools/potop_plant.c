// potop_plant: grows a plant into an object for the game (.pobj), from the
// command line: see tools/plant_grow.h and plant_grow.c for how. A desktop
// tool: it is not part of the game and never ships with it. The same in a
// window, with the plant in view while it changes: tools/plant_editor.
//
//   potop_plant [options] [data/objects/name.pobj]
//
// Options:
//   --recipe FILE                    a recipe (plant_plans/, see tools/plant_grow.h): read first, the
//                                    options after it change it; the object goes to its output then
//   --save-recipe FILE               writes the recipe the options make (and grows the plant too)
//   --all DIR                        (after --data) grows every recipe in DIR again (DIR/*.plant, each to its output)
//                                    and nothing else: after the tool changes, the plants as it makes them now
//   --kind K                         oak, birch, willow, weeping_willow, pollard_willow, bush; spruce, fir, pine, thuja, juniper, mountain_pine;
//                                    cherry, plum, sour_cherry
//                                    (trees: the conifers' or the broadleaved's atlas; deciduous and conifer: oak, spruce);
//                                    tuft, rosette, fern, flower (small plants: small_plants)
//   --seed N                         which one of the kind (1)
//   --height M                       meters, about (the kind's own)
//   --bark FILE                      bark (trees) or stalks (small plants): its color; under data/,
//                                    named by the object (not packed); tiles round and along
//   --bark-normal FILE, --bark-arm FILE
//   --bark-meters M                  how much bark one tile covers (1.5)
//   --stalk-color R,G,B              without a bark picture: the stalks' and branches' color (sRGB 0..1; the kind's)
//   --leaves FILE[,FILE...]          leaves on alpha, the stem at the bottom middle (any picks one at random);
//                                    the blades of tufts, the leaves of rosettes, ferns and flowers
//                                    a picture's months: beside it as name_m1.png .. name_m12.png (see plant_grow.h)
//   --twigs FILE[,FILE...]           sprigs on alpha (trees: the finest branches become cards of them)
//   --heads FILE[,FILE...]           flower heads on alpha (rosettes, flowers)
//   --leaf-size M                    a leaf's length (the kind's own)
//   --leaves-per-branch N            trees: leaves on each of the finest branches (the kind's own)
//   --branches N[,N[,N]]             trees: branches out of the trunk, out of each of those, ... (the kind's own)
//   --branch-length X                trees: the trunk's branches X times as long (1)
//   --twig-scale X                   trees: a twig's length, of the branch it stands for (the kind's own)
//   --crown-start F, --leaves-from F, --droop X, --tip-lift X, --collar X, --root-flare X
//   --trunk-thickness F, --trunk-taper F, --branch-thickness F, --branch-taper F, --thinnest-branch M
//   --mushroom KEY=VALUE (a mushroom's shape or colors, as its recipe names them), --mushroom-template FILE
//   --herb-preset NAME (a small plant: burdock, plantain, dandelion, daisy, foxglove, sunflower, grass_clump: its kind, sizes and shape),
//   --herb KEY=VALUE (a tuft's, rosette's or flower's shape: leaves, leaf_rise, leaf_arch, petiole, stalks_minimum ...)
//                                    trees: the crown's shape (see plant_grow.h; the kind's own)
//   --stems N, --fork-height F, --extra-stem-chance P, --gnarl X, --kinks X,
//   --lean-degrees D, --lean-length F, --straighten F, --wind-side F
//                                    trees: the trunk's shape (see plant_grow.h; the kind's own)
//   --moss X, --tips X, --autumn X, --autumn-colors R,G,B,R,G,B, --falls 0|1,
//   --autumn-months F,L, --leaves-month M, --snow-sag X
//                                    through the seasons (see plant_grow.h; the kind's own)
//   --twig-share S                   trees with both --twigs and --leaves: the share of the finest branches
//                                    that are twigs (0..1; the rest keep their leaves; 0.7)
//   --loose-leaves N                 trees: leaves along the other branches too, N per meter of them (0)
//   --fruit FILE[,FILE...]           trees: fruit pictures on alpha (any picks one at random), seen from the
//                                    side, filling the picture: the end it hangs from at the top for hanging
//                                    fruit (--fruit-hang down), at the bottom otherwise
//   --fruit-shape S                  sphere (cherries, nuts; oval with a width), spindle (cones), pod (flat,
//                                    curved), card (two crossed cards) (sphere)
//   --fruit-size L[,W]               its length along its axis, and width (meters; the width: from the
//                                    picture's shape) (0.03)
//   --fruit-hang H                   down (on its stalk), up (standing on the branch), out (out of it)
//                                    (the kind's: spruce down, fir and thuja up, pine and juniper out, the rest down)
//   --fruit-stalk M                  the stalk's length (0: none) (0)
//   --fruit-cluster N[,M]            N to M together, their stalks from one point (1)
//   --fruit-per-branch X             clusters on each finest branch (or twig), on average (1)
//   --fruit-zone Z                   top (the crown's top), outer (its outside), all (conifers top, the rest outer)
//   --atlas NAME                     another atlas than the kind's (data/textures/NAME.ini)
//   --data DIR                       the game's data directory (data)
// Paths are as seen from where the tool runs.

// (opendir: POSIX)
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plant_grow.h"

static void usage(void)
{
	fprintf(stderr,
		"Usage: potop_plant [--recipe FILE] [options] [data/objects/name.pobj]\n"
		"  --recipe FILE           a recipe to start from (plant_plans/); --save-recipe FILE writes one\n"
		"  --all DIR               grows every recipe in DIR again (DIR/*.plant), and nothing else\n"
		"  --kind K                trees: oak, birch, willow, weeping_willow, pollard_willow, bush; spruce, fir, pine, thuja, juniper, mountain_pine; cherry, plum, sour_cherry\n"
		"                          small plants: tuft, rosette, fern, flower (oak; deciduous = oak, conifer = spruce)\n"
		"  --seed N                which one of the kind (1)\n"
		"  --height M              meters, about\n"
		"  --bark FILE [--bark-normal FILE] [--bark-arm FILE] [--bark-meters M]   (under data/)\n"
		"  --stalk-color R,G,B     without a bark picture: the stalks' and branches' color (sRGB 0..1; the kind's)\n"
		"  --leaves FILE[,FILE...] [--leaf-size M]   (a picture's months beside it: name_m1.png .. name_m12.png)\n"
		"  --leaves-per-branch N   trees: leaves on each of the finest branches\n"
		"  --branches N[,N[,N]]    trees: branches out of the trunk, out of each of those, ...\n"
		"  --branch-length X       trees: the trunk's branches X times as long (1)\n"
		"  --twig-scale X          trees: a twig's length, of the branch it stands for (the kind's own)\n"
		"  --twig-share S          trees with twigs and leaves: the share of the finest branches that are twigs (0.7)\n"
		"  --loose-leaves N        trees: leaves along the other branches too, per meter (0)\n"
		"  --crown-start F         trees: the first branches up the trunk, of its height (0..1; the kind's)\n"
		"  --leaves-from F         trees: foliage only above this much of the height, lower only at the tips\n"
		"  --droop X               trees: the branches bend down under their weight (the kind's)\n"
		"  --trunk-thickness F     trees: the trunk's radius at its foot, of the height (the kind's)\n"
		"  --trunk-taper F         trees: the trunk's radius at its top, of its foot's (the kind's)\n"
		"  --branch-thickness F    trees: a branch's radius at its foot, of its parent's there (the kind's)\n"
		"  --branch-taper F        trees: a branch's radius at its tip, of its foot's (the kind's)\n"
		"  --thinnest-branch M     trees: no branch thinner than this radius, meters (0.002)\n"
		"  --frond-angle D         ferns: the fronds start D degrees up from level (58)\n"
		"  --frond-droop X         ferns: and arch over this much (0.7; 1: much)\n"
		"  --herb-preset NAME      a small plant: burdock, plantain, dandelion, daisy, foxglove, sunflower, grass_clump\n"
		"  --herb KEY=VALUE        a tuft's, rosette's or flower's shape (leaves=9, leaf_rise=30, leaf_arch=0.5, petiole=0.2,\n"
		"                          stalks_minimum=1, heads=8, head_spike=0.4 ...)\n"
		"  --mushroom KEY=VALUE    mushrooms: their shape or colors, as a recipe names them (stem_thickness=0.08,\n"
		"                          cap_color=0.8,0.1,0.05 ...; see README)\n"
		"  --mushroom-template FILE  writes the template a mushroom's texture is laid out as, and stops\n"
		"  --tip-lift X            trees: their tips turn up again (the kind's)\n"
		"  --collar X              trees: a branch's foot swells where it joins, of its radius (the kind's)\n"
		"  --root-flare X          trees: the trunk's foot swells towards the roots (the kind's)\n"
		"  --twig-length M         trees: a finest branch longer than M meters has twigs one after the other along it (0: one)\n"
		"  --stems N               trees: trunks (the kind's)\n"
		"  --fork-height F         trees: they grow as one up to this much of the height (0: apart from the ground)\n"
		"  --extra-stem-chance P   trees: the chance of one stem more (by the seed)\n"
		"  --gnarl X               trees: the trunk and branches wander, swell and narrow (0: smooth)\n"
		"  --kinks X               trees: a branch turns away where another leaves it (0: none)\n"
		"  --lean-degrees D        trees: bent by the wind: the trunk leaves the ground this far from upright\n"
		"  --lean-length F         trees: and goes so for this much of its length\n"
		"  --straighten F          trees: then turns up again (1: upright by its top, 0: not at all)\n"
		"  --wind-side F           trees: the branches on the wind's side shorter and fewer (0..1)\n"
		"  --moss X                the moss on the bark's north side (0..1; the kind's)\n"
		"  --tips X                how much lighter the foliage's tips are (the new growth; the kind's)\n"
		"  --autumn X              how much the foliage turns in the autumn (0: evergreen; the kind's)\n"
		"  --autumn-colors R,G,B,R,G,B   what it turns to (sRGB 0..1, a card between the two; the kind's)\n"
		"  --autumn-months F,L     the autumn: it turns from the start of month F, falls by the end of L (the kind's: 10,11)\n"
		"  --leaves-month M        and is bare until month M (the kind's: 4)\n"
		"  --snow-sag X            how far the branches sag under the winter's snow (of the height, at the crown's edge)\n"
		"  --falls 0|1             whether it falls once turned (the kind's)\n"
		"  --twigs FILE[,FILE...]\n"
		"  --heads FILE[,FILE...]\n"
		"  --fruit FILE[,FILE...]  trees: fruit\n"
		"  --fruit-shape S         sphere, spindle, pod, card (sphere)\n"
		"  --fruit-size L[,W]      meters (0.03; the width from the picture)\n"
		"  --fruit-hang H          down, up, out (the kind's)\n"
		"  --fruit-stalk M         its stalk (0: none)\n"
		"  --fruit-cluster N[,M]   N to M together (1)\n"
		"  --fruit-per-branch X    clusters per finest branch or twig (1)\n"
		"  --fruit-zone Z          top, outer, all (the kind's)\n"
		"  --atlas NAME            another atlas than the kind's (data/textures/NAME.ini)\n"
		"  --data DIR              the game's data directory (data)\n"
		"Leaves, twigs, heads and fruit: pictures on alpha; they go in the atlas by themselves.\n"
		"Paths are as seen from where the tool runs.\n");
}

// "a,b,c" into the pictures (the months chosen by hand of those kept stay)
static void set_pictures(plant_pictures_t *pictures, const char *list)
{
	char copy[PLANT_PATH_LENGTH * PLANT_MAX_PICTURES];
	int count = 0;

	snprintf(copy, sizeof(copy), "%s", list);
	for (char *piece = strtok(copy, ","); piece != NULL && count < PLANT_MAX_PICTURES; piece = strtok(NULL, ","))
	{
		while (*piece == ' ')
		{
			piece++;
		}
		if (count >= pictures->count)
		{
			plant_pictures_add(pictures, piece);
		}
		else if (strcmp(pictures->pictures[count].color, piece) != 0)
		{
			memset(&pictures->pictures[count], 0, sizeof(pictures->pictures[count]));
			snprintf(pictures->pictures[count].color, PLANT_PATH_LENGTH, "%s", piece);
		}
		count++;
	}
	pictures->count = count;
}

// "N,N,N" into numbers; how many
static int split_numbers(const char *text, float *out, int most)
{
	int count = 0;
	const char *cursor = text;

	while (*cursor != '\0' && count < most)
	{
		char *after;
		float value = strtof(cursor, &after);

		if (after == cursor)
		{
			break;
		}
		out[count++] = value;
		cursor = after;
		while (*cursor == ',' || *cursor == ' ')
		{
			cursor++;
		}
	}
	return count;
}

// One recipe's plant, grown and written (the recipe saved first where asked)
static int grow_one(plant_recipe_t *recipe, const char *data_directory, const char *save_recipe)
{
	plant_atlas_t *atlas;
	plant_mesh_t mesh;

	if (recipe->output[0] == '\0')
	{
		usage();
		return 1;
	}
	if (save_recipe != NULL && !plant_recipe_save(recipe, save_recipe))
	{
		return 1;
	}
	atlas = plant_atlas_prepare(recipe, data_directory);
	if (atlas == NULL || !plant_atlas_write(atlas) || !plant_grow(recipe, atlas, &mesh))
	{
		plant_atlas_free(atlas);
		return 1;
	}
	printf("%s, seed %u: %.2f m, %zu vertices, %zu trunk (solid), %zu bark, %zu foliage and %zu fruit triangles (%d fruit)\n",
		recipe->kind, recipe->seed, (double)mesh.height, mesh.vertex_count, mesh.index_count[PLANT_MATERIAL_TRUNK] / 3,
		mesh.index_count[PLANT_MATERIAL_BARK] / 3, mesh.index_count[PLANT_MATERIAL_FOLIAGE] / 3, mesh.index_count[PLANT_MATERIAL_FRUIT] / 3,
		mesh.fruit_count);
	if (!plant_write(recipe->output, &mesh, recipe, atlas, data_directory))
	{
		plant_mesh_free(&mesh);
		plant_atlas_free(atlas);
		return 1;
	}
	printf("Wrote %s\n", recipe->output);
	plant_mesh_free(&mesh);
	plant_atlas_free(atlas);
	return 0;
}

// --all: every recipe in a folder (DIR/*.plant, each to its own output)
static int grow_folder(const char *folder, const char *data_directory)
{
	DIR *directory = opendir(folder);
	struct dirent *entry;
	int made = 0;
	int failed = 0;

	if (directory == NULL)
	{
		fprintf(stderr, "potop_plant: cannot read the folder '%s'\n", folder);
		return 1;
	}
	while ((entry = readdir(directory)) != NULL)
	{
		static plant_recipe_t recipe;
		size_t length = strlen(entry->d_name);
		char path[PLANT_PATH_LENGTH * 2];

		if (length < 7 || strcmp(entry->d_name + length - 6, ".plant") != 0)
		{
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		plant_recipe_defaults(&recipe);
		if (!plant_recipe_load(&recipe, path) || recipe.output[0] == '\0' || grow_one(&recipe, data_directory, NULL) != 0)
		{
			fprintf(stderr, "potop_plant: %s failed (or names no output)\n", path);
			failed++;
			continue;
		}
		made++;
	}
	closedir(directory);
	printf("%d recipes grown again%s\n", made, failed > 0 ? ", some failed (see above)" : "");
	return failed > 0 ? 1 : 0;
}

int main(int argument_count, char **arguments)
{
	static plant_recipe_t recipe;
	const char *data_directory = "data";
	const char *save_recipe = NULL;

	plant_recipe_defaults(&recipe);
	// --all DIR (after --data, where given): every recipe there, and nothing else
	for (int i = 1; i + 1 < argument_count; i++)
	{
		if (strcmp(arguments[i], "--data") == 0)
		{
			data_directory = arguments[i + 1];
		}
		else if (strcmp(arguments[i], "--all") == 0)
		{
			return grow_folder(arguments[i + 1], data_directory);
		}
	}
	// The recipe first, whatever comes before it
	for (int i = 1; i + 1 < argument_count; i++)
	{
		if (strcmp(arguments[i], "--recipe") == 0 && !plant_recipe_load(&recipe, arguments[i + 1]))
		{
			return 1;
		}
	}
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		if (argument[0] != '-')
		{
			snprintf(recipe.output, sizeof(recipe.output), "%s", argument);
			continue;
		}
		if (value == NULL)
		{
			usage();
			return 1;
		}
		i++;
		if (strcmp(argument, "--recipe") == 0) { }
		else if (strcmp(argument, "--save-recipe") == 0) { save_recipe = value; }
		else if (strcmp(argument, "--kind") == 0)
		{
			if (plant_kind_find(value) < 0)
			{
				fprintf(stderr, "No kind '%s' (oak, birch, willow, weeping_willow, pollard_willow, bush, spruce, fir, pine, thuja, juniper, mountain_pine, cherry, plum, sour_cherry, tuft, rosette, fern, flower, mushroom)\n", value);
				return 1;
			}
			snprintf(recipe.kind, sizeof(recipe.kind), "%s", value);
		}
		else if (strcmp(argument, "--seed") == 0) { recipe.seed = (unsigned)strtoul(value, NULL, 10); }
		else if (strcmp(argument, "--height") == 0) { recipe.height = strtof(value, NULL); }
		else if (strcmp(argument, "--leaf-size") == 0) { recipe.leaf_size = strtof(value, NULL); }
		else if (strcmp(argument, "--leaves-per-branch") == 0) { recipe.leaves_per_branch = (int)strtol(value, NULL, 10); }
		else if (strcmp(argument, "--branch-length") == 0) { recipe.branch_length = strtof(value, NULL); }
		else if (strcmp(argument, "--twig-scale") == 0) { recipe.twig_scale = strtof(value, NULL); }
		else if (strcmp(argument, "--twig-share") == 0) { recipe.twig_share = strtof(value, NULL); }
		else if (strcmp(argument, "--loose-leaves") == 0) { recipe.loose_leaves = strtof(value, NULL); }
		else if (strcmp(argument, "--crown-start") == 0) { recipe.crown_start = strtof(value, NULL); }
		else if (strcmp(argument, "--leaves-from") == 0) { recipe.leaves_from = strtof(value, NULL); }
		else if (strcmp(argument, "--droop") == 0) { recipe.droop = strtof(value, NULL); }
		else if (strcmp(argument, "--trunk-thickness") == 0) { recipe.trunk_thickness = strtof(value, NULL); }
		else if (strcmp(argument, "--trunk-taper") == 0) { recipe.trunk_taper = strtof(value, NULL); }
		else if (strcmp(argument, "--branch-thickness") == 0) { recipe.branch_thickness = strtof(value, NULL); }
		else if (strcmp(argument, "--branch-taper") == 0) { recipe.branch_taper = strtof(value, NULL); }
		else if (strcmp(argument, "--thinnest-branch") == 0) { recipe.thinnest_branch = strtof(value, NULL); }
		else if (strcmp(argument, "--frond-angle") == 0) { recipe.frond_angle = strtof(value, NULL); }
		else if (strcmp(argument, "--frond-droop") == 0) { recipe.frond_droop = strtof(value, NULL); }
		else if (strcmp(argument, "--mushroom") == 0)
		{
			char key[64];
			const char *equals = strchr(value, '=');

			snprintf(key, sizeof(key), "%.*s", equals != NULL ? (int)(equals - value) : 0, value);
			if (equals == NULL || !plant_mushroom_set(&recipe.mushroom, key, equals + 1))
			{
				fprintf(stderr, "--mushroom %s: not a mushroom's KEY=VALUE\n", value);
				return 1;
			}
		}
		else if (strcmp(argument, "--herb") == 0)
		{
			char key[64];
			const char *equals = strchr(value, '=');

			snprintf(key, sizeof(key), "%.*s", equals != NULL ? (int)(equals - value) : 0, value);
			if (equals == NULL || !plant_herb_set(&recipe.herb, key, equals + 1))
			{
				fprintf(stderr, "--herb %s: not a small plant's KEY=VALUE\n", value);
				return 1;
			}
		}
		else if (strcmp(argument, "--herb-preset") == 0)
		{
			if (!plant_herb_preset(&recipe, value))
			{
				fprintf(stderr, "--herb-preset %s: none of that name (burdock, plantain, dandelion, daisy, foxglove, sunflower, grass_clump)\n", value);
				return 1;
			}
		}
		else if (strcmp(argument, "--mushroom-template") == 0)
		{
			return plant_mushroom_template(value) ? 0 : 1;
		}
		else if (strcmp(argument, "--tip-lift") == 0) { recipe.tip_lift = strtof(value, NULL); }
		else if (strcmp(argument, "--collar") == 0) { recipe.collar = strtof(value, NULL); }
		else if (strcmp(argument, "--root-flare") == 0) { recipe.root_flare = strtof(value, NULL); }
		else if (strcmp(argument, "--twig-length") == 0) { recipe.twig_meters = strtof(value, NULL); }
		else if (strcmp(argument, "--stems") == 0) { recipe.stems = atoi(value); }
		else if (strcmp(argument, "--fork-height") == 0) { recipe.fork_height = strtof(value, NULL); }
		else if (strcmp(argument, "--extra-stem-chance") == 0) { recipe.extra_stem_chance = strtof(value, NULL); }
		else if (strcmp(argument, "--gnarl") == 0) { recipe.gnarl = strtof(value, NULL); }
		else if (strcmp(argument, "--kinks") == 0) { recipe.kinks = strtof(value, NULL); }
		else if (strcmp(argument, "--lean-degrees") == 0) { recipe.lean_degrees = strtof(value, NULL); }
		else if (strcmp(argument, "--lean-length") == 0) { recipe.lean_length = strtof(value, NULL); }
		else if (strcmp(argument, "--straighten") == 0) { recipe.straighten = strtof(value, NULL); }
		else if (strcmp(argument, "--wind-side") == 0) { recipe.wind_side = strtof(value, NULL); }
		else if (strcmp(argument, "--moss") == 0) { recipe.moss = strtof(value, NULL); }
		else if (strcmp(argument, "--tips") == 0) { recipe.tips = strtof(value, NULL); }
		else if (strcmp(argument, "--autumn") == 0) { recipe.autumn = strtof(value, NULL); }
		else if (strcmp(argument, "--falls") == 0) { recipe.falls = atoi(value); }
		else if (strcmp(argument, "--autumn-months") == 0)
		{
			int first = 0;
			int last = 0;

			if (sscanf(value, "%d,%d", &first, &last) != 2 || first < 1 || first > 12 || last < 1 || last > 12)
			{
				fprintf(stderr, "--autumn-months F,L (months, 1..12)\n");
				return 1;
			}
			recipe.autumn_first_month = first;
			recipe.autumn_last_month = last;
		}
		else if (strcmp(argument, "--leaves-month") == 0)
		{
			int month = atoi(value);

			if (month < 1 || month > 12)
			{
				fprintf(stderr, "--leaves-month M (1..12)\n");
				return 1;
			}
			recipe.leaves_month = month;
		}
		else if (strcmp(argument, "--snow-sag") == 0) { recipe.snow_sag = strtof(value, NULL); }
		else if (strcmp(argument, "--autumn-colors") == 0)
		{
			float c[6];

			if (sscanf(value, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) != 6)
			{
				fprintf(stderr, "--autumn-colors R,G,B,R,G,B (0..1 each, two colors)\n");
				return 1;
			}
			memcpy(recipe.autumn_colors, c, sizeof(c));
		}
		else if (strcmp(argument, "--branches") == 0)
		{
			float numbers[PLANT_MAX_LEVELS];

			recipe.branch_levels = split_numbers(value, numbers, PLANT_MAX_LEVELS);
			for (int k = 0; k < recipe.branch_levels; k++)
			{
				recipe.branches[k] = (int)numbers[k];
			}
		}
		else if (strcmp(argument, "--stalk-color") == 0)
		{
			float c[3];

			if (sscanf(value, "%f,%f,%f", &c[0], &c[1], &c[2]) != 3)
			{
				fprintf(stderr, "--stalk-color R,G,B (0..1 each)\n");
				return 1;
			}
			memcpy(recipe.stalk_color, c, sizeof(c));
		}
		else if (strcmp(argument, "--bark-meters") == 0) { recipe.bark_meters = fmaxf(strtof(value, NULL), 0.05f); }
		else if (strcmp(argument, "--atlas") == 0) { snprintf(recipe.atlas, sizeof(recipe.atlas), "%s", value); }
		else if (strcmp(argument, "--data") == 0) { data_directory = value; }
		else if (strcmp(argument, "--bark") == 0) { snprintf(recipe.bark[0], PLANT_PATH_LENGTH, "%s", value); }
		else if (strcmp(argument, "--bark-normal") == 0) { snprintf(recipe.bark[1], PLANT_PATH_LENGTH, "%s", value); }
		else if (strcmp(argument, "--bark-arm") == 0) { snprintf(recipe.bark[2], PLANT_PATH_LENGTH, "%s", value); }
		else if (strcmp(argument, "--leaves") == 0) { set_pictures(&recipe.leaves, value); }
		else if (strcmp(argument, "--twigs") == 0) { set_pictures(&recipe.twigs, value); }
		else if (strcmp(argument, "--heads") == 0) { set_pictures(&recipe.heads, value); }
		else if (strcmp(argument, "--fruit") == 0) { set_pictures(&recipe.fruit, value); }
		else if (strcmp(argument, "--fruit-stalk") == 0) { recipe.fruit_stalk = strtof(value, NULL); }
		else if (strcmp(argument, "--fruit-per-branch") == 0) { recipe.fruit_per_branch = strtof(value, NULL); }
		else if (strcmp(argument, "--fruit-size") == 0)
		{
			float numbers[2] = { 0.03f, 0.0f };
			int count = split_numbers(value, numbers, 2);

			recipe.fruit_length = count > 0 ? numbers[0] : recipe.fruit_length;
			recipe.fruit_width = count > 1 ? numbers[1] : 0.0f;
		}
		else if (strcmp(argument, "--fruit-cluster") == 0)
		{
			float numbers[2] = { 1.0f, 1.0f };
			int count = split_numbers(value, numbers, 2);

			recipe.cluster_minimum = count > 0 ? (int)numbers[0] : 1;
			recipe.cluster_maximum = count > 1 ? (int)numbers[1] : recipe.cluster_minimum;
		}
		else if (strcmp(argument, "--fruit-shape") == 0)
		{
			static const char *const names[] = { "sphere", "spindle", "pod", "card" };
			int found = -1;

			for (int k = 0; k < 4; k++)
			{
				found = strcmp(value, names[k]) == 0 ? k : found;
			}
			if (found < 0)
			{
				fprintf(stderr, "No fruit shape '%s' (sphere, spindle, pod, card)\n", value);
				return 1;
			}
			recipe.fruit_shape = (plant_fruit_shape_t)found;
		}
		else if (strcmp(argument, "--fruit-hang") == 0)
		{
			recipe.fruit_hang = strcmp(value, "down") == 0 ? PLANT_FRUIT_DOWN : (strcmp(value, "up") == 0 ? PLANT_FRUIT_UP
				: (strcmp(value, "out") == 0 ? PLANT_FRUIT_OUT : -2));
			if (recipe.fruit_hang == -2)
			{
				fprintf(stderr, "No fruit hang '%s' (down, up, out)\n", value);
				return 1;
			}
		}
		else if (strcmp(argument, "--fruit-zone") == 0)
		{
			recipe.fruit_zone = strcmp(value, "top") == 0 ? PLANT_FRUIT_TOP : (strcmp(value, "outer") == 0 ? PLANT_FRUIT_OUTER
				: (strcmp(value, "all") == 0 ? PLANT_FRUIT_ALL : -2));
			if (recipe.fruit_zone == -2)
			{
				fprintf(stderr, "No fruit zone '%s' (top, outer, all)\n", value);
				return 1;
			}
		}
		else
		{
			fprintf(stderr, "Unknown option '%s'\n", argument);
			usage();
			return 1;
		}
	}
	return grow_one(&recipe, data_directory, save_recipe);
}
