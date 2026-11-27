// The scenes the damage tools show: see damage_scene.h

#include "damage_scene.h"

#include <string.h>

#include "pile_scene.h"
#include "test_scene.h"

#define SCENE_COUNT 3

static const damage_scene_t *const scenes[SCENE_COUNT] =
{
	&pile_scene,
	&sandbox_scene,
	&test_scene,
};

int damage_scene_count(void)
{
	return SCENE_COUNT;
}

const damage_scene_t *damage_scene_get(int scene_index)
{
	if (scene_index < 0 || scene_index >= SCENE_COUNT)
	{
		return NULL;
	}
	return scenes[scene_index];
}

const damage_scene_t *damage_scene_find(const char *name)
{
	int i;

	for (i = 0; i < SCENE_COUNT; i++)
	{
		if (strcmp(scenes[i]->name, name) == 0)
		{
			return scenes[i];
		}
	}
	return NULL;
}

const damage_scene_t *damage_scene_default(void)
{
	return scenes[0];
}
