// The pile: Box3D alone, with nothing of the damage model yet, seen
// through the soft renderer. A brick wall, a crib of stacked logs, loose
// logs falling on it, a raft of four logs that is one body until it is
// split in mid air, a steel ball thrown at the wall and an explosion
// under the crib.
//
// It is here to see that the two fit together and to learn what the
// model will lean on: that the same steps give the same world (a frame
// can be gone back to), how a hit and its impulse are read, what shape a
// log should be, that a body can be taken apart in mid flight without a
// jolt.
//
// The balls and blasts of damage_events.h are set off in it too, on top
// of its own film.

#ifndef PILE_SCENE_H
#define PILE_SCENE_H

#include "damage_scene.h"

extern const damage_scene_t pile_scene;
// The same wall and crib with nothing happening by itself: only the balls
// and blasts of the person at the keys (damage_events.h)
extern const damage_scene_t sandbox_scene;

#endif
