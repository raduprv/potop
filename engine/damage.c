// The kinds of damage. See damage.h.

#include "damage.h"

#include <SDL3/SDL.h>

static const char *const DAMAGE_NAMES[DAMAGE_TYPE_COUNT] = { "ballistic", "explosive", "fire", "cold", "poison", "magic" };

bool damage_type_from_name(const char *name, damage_type_t *type_out)
{
	for (int t = 0; t < DAMAGE_TYPE_COUNT; t++)
	{
		if (SDL_strcasecmp(name, DAMAGE_NAMES[t]) == 0)
		{
			*type_out = (damage_type_t)t;
			return true;
		}
	}
	return false;
}

const char *damage_type_name(damage_type_t type)
{
	return type >= 0 && type < DAMAGE_TYPE_COUNT ? DAMAGE_NAMES[type] : "?";
}
