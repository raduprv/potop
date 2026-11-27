// The kinds of damage a hit carries (a bullet's, a blast's, a fire's...):
// the engine only says which; what each does (armour, burning, a bear
// that fears fire) is the game's.

#ifndef DAMAGE_H
#define DAMAGE_H

#include <stdbool.h>

typedef enum
{
	DAMAGE_BALLISTIC = 0,          // bullets, arrows
	DAMAGE_EXPLOSIVE,              // a blast
	DAMAGE_FIRE,
	DAMAGE_COLD,
	DAMAGE_POISON,
	DAMAGE_MAGIC,
	DAMAGE_BLUNT,                  // a heavy thing striking (a ball shot)
	DAMAGE_TYPE_COUNT
} damage_type_t;

// "ballistic", "explosive", "fire", "cold", "poison", "magic", "blunt" (the inis'
// names); false if it is none of them
bool damage_type_from_name(const char *name, damage_type_t *type_out);
const char *damage_type_name(damage_type_t type);

#endif
