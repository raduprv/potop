// What an actor sees: how well it makes out something at a point (the
// player, another actor). Far, behind it, behind the ground or something
// solid (a trunk, a wall, a rock), in the dark (light_probe.h), lying low:
// less, or not at all. The game's animals and guards go by it (what they
// hear: noises.h).

#ifndef SENSES_H
#define SENSES_H

#include "world.h"

// 0 (not seen) .. 1 (right in front of it, in full light): its sight
// (actors.ini sight_meters, sight_degrees) shortened by the dark there and
// by how visible the target is (1: standing; lying low, less)
float senses_sight(world_t *world, int actor_index, const float target[3], float target_visibility);

#endif
