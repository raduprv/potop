// The cabin: the damage model's first scene. A log cabin (butt and pass:
// in each course two logs run through the corners and the two across
// stop against them, the other way round in the next course; three
// beams on top) and a brick wall with mortar, both standing still as
// static bodies until something breaks a link.
//
// The links are laid by rules written for this scene alone (where the
// logs meet, where the bricks touch): not the generator the real
// buildings will have.
//
// Nothing happens by itself: throw balls and set off blasts at it
// (damage_events.h). The numbers are damage.ini's, read at every start.

#ifndef CABIN_SCENE_H
#define CABIN_SCENE_H

#include "damage_scene.h"

extern const damage_scene_t cabin_scene;

#endif
