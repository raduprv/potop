# Weapons

On foot, **1** brings the first weapon up (or puts it away), **2** the
second (the one in hand goes down first); while one is up, the left button
fires, the right button (held) aims and **R** reloads (not the rain). The status line shows
the weapon and what is in the magazine (the ammo is endless for now). There are two for
now, drawn from boxes and prisms until there are real models: an AK-47
(`model = procedural:rifle`) and an SVD with a scope
(`procedural:scoped_rifle`).

Aiming brings the weapon to the middle, its sights at the eye
(`aim_seconds`), and narrows the view `aim_zoom` times (1.35 for the AK);
the mouse turns the view as much slower, the spread and the recoil are
`aim_spread` and `aim_recoil` times theirs. A weapon with `scope_zoom`
(6 for the SVD) shows, once at the eye, the scope's round view (black
round it, a duplex reticle) narrowed that many times; the weapon itself is
not drawn then.

Three files, each documenting its keys at its top:

- `data/weapons.ini`: one section per weapon: its ammo, `fire = semi` or
  `auto`, rounds per minute, magazine, reload and ready time, spread (how it
  grows shot by shot and comes back), recoil (up and sideways, kicking the
  view), the muzzle's effect, the tracer's effect and how often (every Nth
  round), the model.
- `data/ammo.ini`: one section per kind of ammo: muzzle speed, mass, drag
  (1/m: the air's slowing goes as drag × speed²), damage, push (how hard
  it shoves a loose body: its momentum times this), how long it may fly.
- `data/impacts.ini`: what a hit throws up, by what it hit: the ground's
  materials, the objects' categories, the water, with one fallback for the
  ground and one for objects.

The effects are sections of `data/particles.ini`: `muzzle_flash`, `tracer`,
`impact_dirt`, `impact_rock`, `impact_wood`, `impact_water`. A tracer starts at the muzzle as drawn, beside the bullet's way: from the eye, a streak going straight away would be only a dot.

The bullets are real projectiles (`engine/projectiles.c`, up to 512 at once):
gravity, the air's drag against the wind (the plants' wind, 0..1 as up to
10 m/s), and the water, which stops one within a meter or two. Each step
(at most 1/120 s) the way flown is checked against the heights and against
the physics' shapes, so a fast one does not go through a thin trunk; a
loose body it hits is pushed. Each hit is also kept for the game (what, where, how fast, the damage as
the energy left goes: `projectiles_take_hits`); what the damage does is up
to the game. Skills change a weapon through `weapons_set_modifiers` (damage,
spread, recoil, reload time, fire rate, muzzle velocity, a bigger magazine).

The weapon in hand (`engine/viewmodel.c`) is drawn last, in the camera's own
space with its own field of view (52°) and its depth squeezed into the
nearest sliver, so it never goes into a wall. It comes up and goes down,
kicks back with each shot, dips for a reload and bobs as you walk; lit by
the sun and the sky. The crosshair is drawn with it.

`--map-test` checks that the weapon fires as fast as weapons.ini says and
that a bullet fired down from 300 m lands on the ground (or the water on it).

Actors are hit by their bodies as posed: each bone has a box (in its own
space, around the vertices it moves most, made when the model loads) that
moves with the animation; the bullet stays in the first one it meets
(`impact_flesh`, the `[flesh]` section with `actors = true`). The hit says
which actor, which bone and where on the body by the bone's name (head:
head, mouth, jaw, ears, eyes; limb: legs, arms, wings, tail, feet, toes;
else body). `actors_hurt` plays its `pain...` action (then back to what it
did), `actors_kill` its `die...` action: it stops for good, a flyer drops
out of the sky. What a hit does is the game's; the game (game/) stands in for
it: 100 health (flyers 30), a head takes 2.5 times the damage, a limb 0.6,
each hit flinches, none left kills (the log says each hit). Eternal Lands'
animals have the flanks on the leg bones, so many body shots count as
limbs.

`--map-test` also shoots a bear from 4 m and checks the bullet stops in it
and it dies.

Not yet: decals (bullet holes), sounds, melee.
