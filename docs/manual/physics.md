# Physics

Objects are rigid bodies in Box3D (see Third party); the water pushes them.
Box3D knows nothing about the water: every simulation step the game works
out what the water does to each body and hands it over as forces.

The physics runs at a fixed rate (30 steps a second) while frames come
faster, so moving bodies are drawn between where the last two steps left
them (position and rotation blended); otherwise a pushed barrel would jump
every few frames. A moving body found under the ground is put back on top,
and the log says what it was, how fast it moved and spun, and whether the
ground tile under it existed (for tracking down why).

* **Buoyancy:** each body has 9 to 27 sample points spread over its shape,
  each standing for a share of the volume it displaces (its mass over its
  density). A point under the surface pushes up with the weight of the water
  its share displaces, at that point, so logs roll and boats tilt the way
  they float. What floats and what sinks follows from the density alone.
* **Drag:** water flowing past a point pushes it with
  1/2 * water density * `drag_coefficient` * area * speed squared, against
  the speed between the water and the point. Under water things also stop
  spinning quickly.

Which objects take part is set per type in `objects.ini` with `physics`:

* `none`: only drawn (candles, cobwebs).
* `flood`: no body, but flowing water lays it down in the flow's direction
  (`flood = flatten`) or tears it out (`destroy`) once it flows faster than
  `flood_speed_meters_per_second` in water at least `flood_depth_meters` deep.
  Flattened plants stand up again over `flood_recover_seconds` once the water
  is gone. No type uses it now: a flood rarely flattens grass for good, and
  doing it at once looked wrong. Grass, flowers, reeds and crops are `none`.
* `static`: a solid body that never moves: houses, tents, walls, docks, wells,
  and also trees, bushes, cacti and fences (a river in flood does not tear
  out a forest; the anchor stays in the code for a real catastrophe later).
  Static things block the water (`blocks_water`, on by default for them; off
  for trees, tents, wells, docks, bridges, fences and signs): they are
  written into the water simulation as walls, the highest point of the model
  over each cell (a pointed roof sheds the water), so a flood splits around a
  house, or rises over it. A wet film on a roof is not drawn.
* `body`: a solid body that moves: logs, barrels, crates, haystacks, carts,
  boats, rocks. It lies loose, asleep, and the water takes it as soon as it
  can. With `anchor_newtons` it would stand still until the water or a hit
  pulls harder than the anchor, then come loose (trees tip over,
  `falls_when_uprooted`); nothing uses that now.

The collision shape is made from the model (`shape = auto`): a capsule for
long things (logs, boats, planks), a convex hull of the model otherwise;
`sphere`, `capsule`, `hull` or `mesh` (the model's own triangles, static only)
override it. Standing objects collide with their solid triangles only:
not leaves (cut out by alpha) nor passable materials (`OBJECT_MATERIAL_PASSABLE`:
a plant's branches, `potop_convert --no-collide`); one with nothing solid
(a flower) is in no one's way. Loose and floating, the shape wraps it all. The mass comes from `mass_kilograms` (at size 1, with the size
cubed) or from the shape's volume times the density.

Breakable things (`damage_newtons`) go from intact to damaged to destroyed
when the water or a hit pushes harder than that, with `damaged_model` and
`destroyed_model` if those exist (a destroyed one without a model is gone),
throwing out `debris_count` pieces of the `debris` types. Nothing is
breakable yet: the damaged and destroyed models do not exist. Houses are
pushed from the outside (they keep the water out): the difference of the
water's weight on opposite sides plus the flow against them.

Bodies cost next to nothing while they sleep: a sleeping body is only
looked at when the water around it moves (an active water tile), and Box3D
wakes whatever a moving body hits. A loose object is drawn where its body
is, kept out of the 32 m grid (culled one by one), and stays where it ends
up, on land too, until the player does something about it; one that leaves
the map is gone. The ground is a Box3D height field per terrain chunk, made
only where something moves; tools and stamps redo it at once, erosion only
where it changed the ground more than `terrain_resync_meters`.

A 1 km map has about 24 000 bodies (trees, bushes, rocks, buildings, things
lying around): made in a quarter of a second, about 0.3 ms per step while
everything sleeps. Saved maps keep what the physics did (loose objects where
they lie, flattened plants, damage).

The **Object** tool (Nature in the brush panel) places one object of a
chosen type per click. Loose things fall from the "Drop from" height (3 m to
start with), awake; at 0 they are put down asleep.
