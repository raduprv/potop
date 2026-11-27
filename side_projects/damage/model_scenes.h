// The scenes built on the damage model (damage_model.h). Both stand
// still as static bodies until something breaks a link, and nothing
// happens in them by itself: throw balls and set off blasts at them
// (damage_events.h). Their numbers are damage.ini's, read at every start.
//
// cabin: a log cabin (butt and pass: in each course two logs run through
// the corners and the two across stop against them, the other way round
// in the next course; three beams on top) and a brick wall with mortar.
//
// support: the three cases the support pass was made for. A wall on two
// piers with a pane of glass under its middle (knock one pier out and the
// wall tips onto the pane, which then has all of it: the pane is
// crushed, the wall hangs from the other pier for a moment and falls);
// a roof whose beams lie on a wall at one end and on a lintel on two
// posts at the other (knock a post out: the lintel cannot stand on one,
// then the beams hold the roof from one end alone, and it pulls the wall
// over); a beam on three pillars with a heavy crate on one end (knock
// out the pillar under the crate: the far pillar's link is pulled up,
// then the beam tips on the middle one).
//
// The links are laid by rules written for these scenes alone (where the
// logs meet, where the bricks touch): not the generator the real
// buildings will have.

#ifndef MODEL_SCENES_H
#define MODEL_SCENES_H

#include "damage_scene.h"

extern const damage_scene_t cabin_scene;
extern const damage_scene_t support_scene;

#endif
