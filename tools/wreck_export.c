// The test bench's buildings (side_projects/damage's scenes: house,
// cabin, support, bend) written out as .wreck files for the game
// (engine/wrecks.h): each scene is built as the bench builds it, and the
// model it made is written down, its pieces and links, centered on its
// middle with its foot on y 0.
//
//   tools/wreck_export SCENE OUTPUT.wreck
//
// Run from the project's top directory: the scenes read
// side_projects/damage/damage.ini, from there.
//
// The model is caught as the scene finishes it (wreck_model_finish is
// wrapped at link time: -Wl,--wrap=wreck_model_finish), so nothing of the
// bench is changed for this.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "damage_scene.h"
#include "wreck.h"

static wreck_model_t *caught_model;

void __real_wreck_model_finish(wreck_model_t *model);

void __wrap_wreck_model_finish(wreck_model_t *model)
{
	caught_model = model;
	__real_wreck_model_finish(model);
}

static const char *const MATERIAL_NAMES[WRECK_MATERIAL_COUNT] = { "wood", "brick", "glass" };
static const char *const LINK_NAMES[WRECK_LINK_TYPE_COUNT] = { "nails", "mortar", "foundation", "glazing", "notch" };

int main(int argument_count, char **arguments)
{
	const damage_scene_t *scene;
	FILE *file;
	float minimum[3] = { 1e30f, 1e30f, 1e30f };
	float maximum[3] = { -1e30f, -1e30f, -1e30f };
	float middle[3];
	int i;

	if (argument_count != 3)
	{
		fprintf(stderr, "Usage: %s SCENE OUTPUT.wreck (house, cabin, support, bend)\n", arguments[0]);
		return 1;
	}
	scene = damage_scene_find(arguments[1]);
	if (scene == NULL)
	{
		fprintf(stderr, "No scene '%s'\n", arguments[1]);
		return 1;
	}
	file = fopen(arguments[2], "w");
	if (file == NULL)
	{
		fprintf(stderr, "Cannot write %s\n", arguments[2]);
		return 1;
	}
	// (the scenes read damage.ini where they stand)
	if (chdir("side_projects/damage") != 0 || !scene->open() || caught_model == NULL)
	{
		fprintf(stderr, "The scene '%s' did not make a model\n", arguments[1]);
		fclose(file);
		return 1;
	}
	// Its middle across, its foot: where it will stand
	for (i = 0; i < caught_model->piece_count; i++)
	{
		const wreck_piece_t *piece = &caught_model->pieces[i];
		float reach = piece->shape == WRECK_PIECE_LOG ? fmaxf(piece->radius, piece->half_length)
			: sqrtf(piece->half_extents.x * piece->half_extents.x + piece->half_extents.y * piece->half_extents.y
				+ piece->half_extents.z * piece->half_extents.z);
		float at[3] = { piece->local.p.x, piece->local.p.y, piece->local.p.z };
		int k;

		for (k = 0; k < 3; k++)
		{
			minimum[k] = fminf(minimum[k], at[k] - (k == 1 ? 0.0f : reach));
			maximum[k] = fmaxf(maximum[k], at[k] + (k == 1 ? 0.0f : reach));
		}
	}
	middle[0] = 0.5f * (minimum[0] + maximum[0]);
	middle[1] = 0.0f;
	middle[2] = 0.5f * (minimum[2] + maximum[2]);
	fprintf(file, "; %s, from side_projects/damage's scene of that name (tools/wreck_export)\n", arguments[1]);
	fprintf(file, "; %d pieces, %d links. See engine/wrecks.h for the form.\n\n[pieces]\n", caught_model->piece_count, caught_model->link_count);
	for (i = 0; i < caught_model->piece_count; i++)
	{
		const wreck_piece_t *piece = &caught_model->pieces[i];
		const b3Transform *local = &piece->local;

		if (piece->shape == WRECK_PIECE_LOG)
		{
			fprintf(file, "log = %s, %.4f %.4f %.4f, %.6f %.6f %.6f %.6f, %.4f %.4f\n", MATERIAL_NAMES[piece->material], local->p.x - middle[0],
				local->p.y - middle[1], local->p.z - middle[2], local->q.v.x, local->q.v.y, local->q.v.z, local->q.s, piece->radius,
				piece->half_length);
		}
		else
		{
			fprintf(file, "box = %s, %.4f %.4f %.4f, %.6f %.6f %.6f %.6f, %.4f %.4f %.4f\n", MATERIAL_NAMES[piece->material],
				local->p.x - middle[0], local->p.y - middle[1], local->p.z - middle[2], local->q.v.x, local->q.v.y, local->q.v.z, local->q.s,
				piece->half_extents.x, piece->half_extents.y, piece->half_extents.z);
		}
	}
	fprintf(file, "\n[links]\n");
	for (i = 0; i < caught_model->link_count; i++)
	{
		const wreck_link_t *link = &caught_model->links[i];

		fprintf(file, "link = %d, %d, %s, %.4f %.4f %.4f\n", link->piece_a, link->piece_b, LINK_NAMES[link->type], link->local_point.x - middle[0],
			link->local_point.y - middle[1], link->local_point.z - middle[2]);
	}
	fclose(file);
	printf("%s: %d pieces, %d links\n", arguments[2], caught_model->piece_count, caught_model->link_count);
	return 0;
}
