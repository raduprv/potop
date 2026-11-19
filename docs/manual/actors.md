# Actors

Actors are animated creatures (a bear and a falcon for now; people, ducks
and the rest later). Their types are in `data/actors.ini`, one section
each, naming a `.pact` model in `data/actors/`. K puts one under the
cursor (Ctrl+K picks the type), where it plays its idle action; J and
Shift+J switch every actor to its model's next or previous action (a debug
key, to see them all). A left click on an actor (on it, a bird up in the
air too, or within 2 m of where the click meets the ground) selects it,
with a circle around it; from then on each left click sends it there (the
tools do not get those clicks), until escape lets go of it.

Sent somewhere, an actor finds its way (`engine/navigation.c`) over a grid
of 1 m cells. A cell can have more than one surface to stand on: the
terrain, and the tops of big objects, a bridge's deck, a dock, a flat rock
(up to three of those per cell), each with the free room above it up to
the next solid thing. They come from the objects' collision triangles (the
physics' shapes), placed, turned and scaled, sampled every eighth of a
meter: triangles facing up (less steep than about 50 degrees) are
surfaces, the rest only take room. A surface also keeps its height per
quarter meter of its cell, for feet: beside a rock that takes part of a
cell is the ground, not the rock's top. A top covering less than a quarter
of its cell (a pointed rock's tip) is no surface; the cell is in the way. Only the lowest 4 m of an object count, so a tree's
trunk is in the way and its crown is not; objects smaller than
`navigation_object_meters` across ([simulation], 0.3 m) are left out, so
actors walk through pebbles. A wall takes exactly the room it
has, a bridge can be crossed on top and, if there is room, under.

Where an actor's body fits is gathered into rectangles (the navigation
mesh: per 64 m of map and per width of actor), and a search is A* over
those, through the openings between them. The mesh is made from the
quarter meters, not from the cells: a quarter meter of a surface is in
it where a body of that width can have its middle there, with all of the
body on something within a step. So it ends a body's half width (a
little less: the body may brush) from a wall, a post, the open edge of a
terrace; a gap is in it if the body gets through, and the strip of
ground beside a porch is ground like any other. Its rectangles are in
quarter meters (up to 16 m a side on open ground, small ones round a
house); a region's are made when a search first gets there (about a
millisecond of open ground, a few with a house on it) and again when
something in the region or by its edge changes. An actor can be on a
surface with at least its
`height_meters` of room (by default its model's height), not in water too
deep or too fast for it; it gets from one rectangle to the next if the
height between them is within its `step_meters` (an object's top: a
stair's tread) or its `maximum_slope_degrees` (the ground). Steep ground and water cost more, so it takes a
detour rather than the hill or the lake when the detour is not too long.
The estimate of the way left is weighed a little over the truth (by 1.3),
which keeps the search heading for the goal, for paths at most that much
longer than the best. The search gives up after an eighth of
`path_search_cells` rectangles ([simulation], 100000) and then heads for
the reached one nearest the goal. A search across a meadow is a few
rectangles, a few hundredths of a millisecond; round a village a few
tenths. The way through the rectangles is pulled tight into at most 32
waypoints; a longer path is searched on from its last waypoint. A goal
where its body's middle cannot be (right by a wall, by a table) is taken
to the nearest place it can, up to 0.75 m off and level with the goal
(not the ground beside the porch the goal is on). An actor that stands
on no rectangle (by a wall, pushed onto a rock, into a corner too tight
for it) starts from the nearest one, one it has a straight way to if
there is one within 2 m, any within 8 m; with none that near it has no
way.

On the way it steers for a point a little ahead on its path, turning
as fast as its pace lets it: `pivot_degrees_per_second` standing (90 by
default: about on the spot in two seconds), down to
`turn_degrees_per_second` at its walking pace and faster. It goes no
faster than it can turn onto that point (a running cow slows where its
turn does not fit, instead of going round the point in circles), and
where its body has no room for the turn's arc (a narrow terrace, a
stair's top) it turns on the spot first and then goes straight. It
leaves its path's corners (for the point ahead, to cut a corner) only
where its body fits all the way, by the quarter meter; where it does not
fit straight to its next corner (a post beside it) it finds a way round
over the quarter meters at once. A way the search finds is gone over
by the quarter meter before the actor sets out: stairs with treads
higher than its `step_meters`, a beam lower than it is tall are left
out and another way looked for, or the actor goes as near as it can get
and stops there (should it meet such a place on its way all the same,
it gives the walk up within a second or so). It slows for a sharp
corner as it comes to it.

An actor with `jump_up_meters` or `jump_down_meters` (actors.ini; 0: it
does not jump) gets up onto and down off what it cannot step: a
terrace's edge. Its search takes such an edge where the way round (the
stairs) is longer by more than about 2 m and 2 m per meter of height,
where it can stand before and beyond the edge with room for its body and
nothing stands on the edge (no jumping a balustrade). It walks to the
spot before the edge (its radius and 0.2 m off it), stops, turns to face
straight across, gathers, goes over on an arc and lands as far beyond
the edge (further out when it drops), then goes on. Its clips, if its
model has them: `jump_start` (from the idle pose), `jump_air` (a pose
held, about level: the engine tilts the body by how it rises and falls,
round its middle) and `jump_land` (from the first touch to the idle
pose), all on the spot; its feet are let go of the ground in the air and
taken up again as `jump_land` plays. How the clips meet in the air is
the type's to say, as they were made: `jump_takeoff_pitch_degrees` and
`jump_landing_pitch_degrees` (the pitch `jump_air`'s pose has where
`jump_start` ends and where `jump_land` begins: the body goes evenly
from the one to the other), `jump_center_meters` and
`jump_center_ahead_meters` (its body's middle, which it is pitched
round: above the model's root and ahead of it in `jump_air`'s pose) and
`jump_start_center_meters`, `jump_land_center_meters` (how high over
the ground that middle is at `jump_start`'s end and at the first touch:
the arc is the middle's, from the one to the other). Without the clips
it goes over as it walks. It walks slower uphill and through water deeper than `wade_depth_meters`,
and swims where the water is deeper than `swim_depth_meters`: afloat, its
body that deep under the surface, level, at `swim_meters_per_second`, with
the walk animation (there is no swimming one). The walk plays at the pace
of its speed (`walk_cycle_meters`, the ground one loop of it covers; 0
plays it as it is at walking speed). Running water carries it, wading a
little, swimming fully, moving or not; a current faster than it swims is
a wall for its path.

Animals that walk without the navigation grid (the farm's: cows, sheep,
pigs, hens) turn going, not on the spot: in a sharp turn they keep
`turn_walk_fraction` of their walk and go round an arc (0.4 by default for
one on four legs, `ground_align` above 0). Standing, they turn at most
`spot_turn_degrees` (20 by default on four legs; 0: no limit) from where
they last went, whatever turns them: a look at a feeder, a step aside, a
bump from another. Only where an arc's first step is blocked (a fence in
front) do they still turn on the spot.

The map changes under its feet, so it keeps an eye on the path: twice a
second it looks at the stretch it can see (`vision_meters`). The map is
split into regions of 128 m, built again when their terrain moved by more
than 5 cm or their objects changed, and stamped when they are. They are
built lazily, only when used: a search builds the stale regions along the
straight line to its goal (and one either side), a look ahead those of the
stretch it sees; erosion far from any actor costs nothing. A new map is
built whole at once. Only if a region along that stretch was stamped
since its last look, or has running water, does it check the cells, and
only if the way is actually blocked does it search again. A change further
on is found when it gets there, like it would be. Stuck for three seconds
without getting half a meter further, it searches again, and gives up
after three tries. What happens comes back as events (arrived, as near as
it gets, no way), shown on the screen for now.

For finding out why an actor goes where it goes, every search is logged:
who, why (sent, blocked ahead, stuck, a long path going on), from where to
where, the result, the waypoints, how many surfaces it looked at and how
long it took. When the goal cannot be reached, the start and the goal are
described: the surface, the room above it and which object takes it, the
steepest step to a neighbor against the actor's reach, the water. The same
when the path ahead turns out blocked (where, and by what) and when the
actor is stuck. It does not test every step it takes: it follows its path
(the rules for where it can go live in one place, the navigation), and
`actors_mark_path_dirty` makes it search again at once, for the physics to
call when something hits it or falls across its way. Caught by water too
fast to get out of (no path at all), an actor keeps its goal and tries
again every second, letting the water carry it meanwhile; it gives up only
once it is on dry ground and still has no way. F8 shows the navigation over
the terrain: objects in the way red, too little room orange, too steep on
every side yellow (faint yellow: on some sides, so only along the slope),
object tops to stand on blue, water it cannot pass violet, and the moving
actors' paths in green with white waypoints, as the first actor type in
actors.ini sees it. Only solid materials count: leaves (textures cut out
by alpha) and passable ones (a tree's branches) are left out, so actors
brush through bushes and under crowns.

On slopes an actor leans with the ground under its feet: the terrain is
sampled at the front, back, left and right of the feet (the lowest tenth
of the model in its rest pose) and the actor tilts by the slopes between
them, up to about 35 degrees, and stands on the plane they make.
`ground_align` (actors.ini) says how much: 1 for animals on four legs, 0
for people, who stay upright and stand on the lowest of the samples so no
foot floats.

Then the feet are put on objects (inverse kinematics). A leg is a foot
bone and the two above it (thigh and calf): every bone whose name starts
with foot, or those `ik_feet` names (actors.ini; `ik = false` turns it
off). Each frame the surface under each foot is looked up where the
animation put the foot (the navigation's surfaces, to the quarter meter),
with its slope over 30 cm; each foot looks near the height it was on, and
where it is over the map comes from the yaw alone (the lean must not move
it, or a foot by a rock's edge goes on and off it every frame). On the
terrain nothing more happens: the body leans with the slope and the legs
move as animated (bending them for every bump of the terrain made a bear
walking on a slope look drunk). On an object's top (a deck, a boulder)
the leg bends: the foot goes as high above that top as the animation
lifts it above the body's own ground plane at the same point, so a step
stays a step and an even deck changes nothing; the thigh and calf bend in
the plane the knee was bending in (two bones, solved exactly; a target out
of reach straightens the leg towards it), and a foot that is down turns
with the top's slope, up to about 35 degrees, less as it lifts (15 cm up:
not at all). Stepping on and off, the bending fades in and out. The body's
height and lean and the feet's ground follow what is found with a time
constant of 80 ms, so a foot stepping onto a boulder does not jerk it.
Swimming, the feet let go as the water deepens. It costs a few hundred
operations per leg.

On slopes an actor also walks slower uphill: its speed is divided by
1 + 2 x the rise per meter (on 20 degrees, 0.36: 58 % of its speed), and
the walk plays at the pace of its speed (there, at 0.58).

The surfaces for the feet are the navigation's, so what is not a surface
there is not stepped on. Objects under `navigation_object_meters` across
([simulation], 0.3 m) are walked through, pebbles.

What a bigger object is to those that walk goes by its footprint and its
mass (or by `navigation = wall|round|soft` in objects.ini, for the odd
one). A module's parts, and an object on its own whose footprint's
longest reach (what of it stands lower than a man) is over
`navigation_wall_meters` ([simulation], 1.2 m), are walls: a fence's
segment is thin, but 2 m long. Under that it is a small thing, gone
round: a trunk, a boulder. A small thing that is a body lighter than
`navigation_soft_kilograms` (20 kg), a bucket, is soft. Small and soft
things are also kept as capsules (two ends and a radius, fitted to the
model's collision shape: one for a straight trunk, a few for a crooked
one); F4 draws them, orange and blue. The small things are left out of
the mesh's rectangles (`navigation_small_out`, [simulation], on), so a
forest is a few big rectangles instead of one round every tree, and a
way found over them is then led round each thing it crosses: the
capsule cut to the walker's height (a leaning trunk is over a hen's head
by its root, not over a man's), laid on the ground, a circle round it
swollen by the walker's half width; the way touches the circle on the
side with less of a turn, a point per quarter turn. Things nearer one
another than the walker is wide (a row of trunks) are a wall to its
width in the mesh: the search goes round the row. Further than
`navigation_detail_meters` (250) from the player the ways are not led
round small things. A straight line (a chase) that crosses a small
thing is not passable: a way is looked for.

A walker on its way keeps to the rectangles too (`navigation_follow_mesh`,
[simulation], on): a step is made if it ends on a rectangle of its
width, is not deeper into a small thing, and the line there is one it
can walk (a step up it can take, a door open). Refused, it slides round
the small thing, back to the corner it cut, or along the rectangle's
edge; refused for a fifth of a second, its way is searched again from
where it stands. The height under its feet is the terrain's, or the
rectangle's on an object. With the setting off it follows over the
quarter meters, as before (fine ways, body points).

A door opened or shut builds nothing again: the cells of a doorway (and
under a blocker) are rectangles of their own, and a search asks each
whom it stops now. Only a door or a blocker that is new, gone or moved
has the tiles there built again.

Soft things are no surface
and take no room: each cell keeps the height of the highest one in it,
and each actor decides by its size. Lower than its step, it walks over
it; lower than itself, the cell is passable but costs
`small_object_detour_meters` (10) more, so it goes around when the detour
is shorter than that and through it when it must (a field of stones);
taller than itself, it is a wall (a rabbit and the same stone). Pulling
the path tight never takes it through a stone the path went around. F8
shows those cells cyan (red when they are walls for the actor). Objects
bigger than that are surfaces (stood on, the feet on them) or take room,
as before; a top covering less than a quarter of a cell (a pointed rock's
tip) is no surface, so such rocks are gone around. On the 1 km map that is about 23 000 objects in the
navigation (the small ones apart); with the finer sampling the navigation
of that map is built in about 0.55 s.

An actor plays one action at a time; switching crossfades from the old
pose to the new over `blend_seconds` (actors.ini). Looping actions repeat;
an action that plays once goes back to the type's `idle_action` when it
ends, except the ones named die..., which hold their last pose. Poses are
computed on the CPU every frame (the clip's two nearest frames,
interpolated), as one matrix per bone.

Actors cast shadows only into the near shadow map. The shadow maps are
cached (the sun does not move), so while actors stand within the near
map's square it is redrawn every `actor_shadow_update_seconds` ([render],
0.1 s by default) for their shadows to follow: that redraws the terrain
and objects around the target too, so it costs. 0 turns it off (their
shadows then stay where they were last drawn).

## Flyers

A type with `flies = true` (the falcon) gets around in the air. It starts
on the ground; sent somewhere, it takes off and follows a flight path. The
navigation keeps, per cell, the highest thing there: the terrain, or the
top of any standing object, whole (a tree's crown too, from its box; loose
objects aside). A flyer goes anywhere under its ceiling, the higher of the
ground where it starts and where it goes plus `fly_ceiling_meters` (150):
its path is the straight line when nothing on it rises higher, else A*
over blocks of 4 x 4 m around what does (a mountain range), climbing
costing a little, pulled tight. Searches are cheap: a detour around a ridge
took 450 blocks and 0.2 ms.

How high it flies is its own: a cruising height above the ground, picked
at random between `fly_height_minimum_meters` and
`fly_height_maximum_meters` (15 and 60) every 4 to 12 seconds, so it is
sometimes up in the sky and sometimes comes down; never less than what it
sees ahead plus 5 m (three seconds of flight ahead: it climbs before a
ridge, not into it). It climbs at `climb_meters_per_second` (4) and dives
twice as fast, points its nose where it goes and banks into turns (the lean
that turns it at that speed, up to 45 degrees). It flies at
`fly_meters_per_second` (12), slower in tight turns, flapping (its walk
action), gliding down (`glide_action`) when it sinks.

At the goal it circles over it (`circle_radius_meters`, 12), going up and
down with its cruising height, if the ring around it is clear low enough:
within `fly_height_maximum_meters` of the goal's ground. If not (a narrow
valley, a dense forest), it comes down on the nearest spot within 40 m
that is dry, open (nothing standing there or within 1.5 m) and has room
for it: straight in along a glide of 0.4 m down per meter, or round over
the spot, sinking, when it is too high for that. On the ground it plays
its idle action. Water reaching it there (a flood) makes it take off and
circle, or land again on the nearest dry spot. Dying (an action named
die...) in the air, it drops.

Eternal Lands' birds fly by their animations: the root bone lifts the body
a meter or two. A flyer is drawn that much lower, so it is where it is and
stands on the ground. The falcon came from Eternal Lands' bird_falcon.xml
with two clips the file does not use added (`bird_fly2.caf` as glide,
`bird_idle1.caf` as hover); it has no animation for sitting, so on the
ground it plays its fight idle for now.

## The .pact format

A `.pact` file holds everything about one actor: mesh, skeleton,
animations (sampled at 30 per second), the names of its actions (walk,
idle, die... each a clip, looping or played once) and its texture,
already decoded (phones have no DXT). The layout is in
`engine/actor_format.h`. The game reads only this format; other formats are
converted on the desktop:

```sh
make tools
tools/potop_convert path/to/actor_defs/bear_1.xml data/actors/bear.pact
tools/potop_convert --dump data/actors/bear.pact
```

The converter reads Eternal Lands actor definitions: the XML names the
Cal3D skeleton (.csf), mesh (.cmf) and animations (.caf), and the DDS skin.
Their paths are relative to the Eternal Lands data directory, by default
the parent of the XML's directory (`--root DIR` sets it); a file not found
there is looked for by name next to the XML. Only binary Cal3D files of
version 700 are read so far. On the way it turns the axes (Z up to Y up),
sorts the bones parents first, keeps the four heaviest bone influences per
vertex and checks the skeleton against the file's own bind pose (it prints
the largest difference; anything above a millimeter means something was
read wrong).
