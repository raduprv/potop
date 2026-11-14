# Picking and highlighting

`objects_raycast` lists the objects a ray goes through, nearest first. It
looks only in the 32 m grid cells along the ray (and their neighbours: a
house or a tree reaches over from the next one) and at the loose objects;
for each, a ball around it first (most are nowhere near), then its box in
its own space, then its triangles, so a tree's crown counts and the air
around it does not (tens of microseconds for a 50 m ray over a wooded map).
`actors_raycast` does the same with the actors' boxes; the game merges the
two lists by distance.

Two kinds of ray: `OBJECTS_RAY_ALL` sees everything, grass too (Alt, the
editor's tools such as Effects), `OBJECTS_RAY_INTERACTIVE` only what the game
can do something with: objects with `OBJECT_STATE_INTERACTIVE`, actors with
`interactive`. What interacting means is the game's business; the engine
only knows the flag. Until the game sets it through its API, placeholder
values: objects with a physics body or in a layout, and every actor. On
foot, what the walker looks at within `interact_meters` ([tools], 6 m) and
is interactive glows softly blue, when `interact_highlight` is on (off by
default; Settings, Camera: "Glow what is in reach"). The flag is not saved with
maps yet.

One object
(`objects_set_highlight`) and one actor (`actors_t.highlight_id`) can be
drawn highlighted: a glow strongest at their outline, pulsing, in a color
of choice, in the normal drawing pass (an object's instance data carries the
flag; no second pass). Alt uses it for debugging; the game will use it for
selection.

The code side of particles: `particles_play(particles, effect, x, y, z)` or
`particles_play_on(...)` with a box, `particles_stop(id)`.
