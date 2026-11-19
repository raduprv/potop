# Navigation: the plan agreed on Nov 18-19 2026

Radu and Claude (Fable 5.2) talked the navigation through after the mesh
was rebuilt from the quarter meters (commit 96e3069). This is what was
decided, so that a later session does not have to guess. Radu's words
where they matter are in quotes. tests/navigation/README.md has how
things work today; this file is where they are going.

## Why

Radu plays on maps of 4096 m, maybe 8192 m later, with animals and
people living all over the map. Today the navigation keeps, for the
whole map: a grid of 14 bytes per 1 m cell (235 MB at 4096, about 1 GB
at 8192), the surfaces by the quarter meter where objects are, and per
mesh tile and class a table "cell -> rectangle" of 16 KB whatever is on
it (about 200 MB at 4096 if every tile is built). Round trees every
trunk cuts the mesh into small rectangles: a forest tile would be
thousands of them. "We do not want 4 GB of navmesh for a 4096 map."

The other engines (looked up that night): Horizon Zero Dawn and Death
Stranding make the mesh at run time only round the player; Unreal's
World Partition bakes it and streams it in chunks; trees are kept out of
the mesh or made cheap. Nobody keeps a fine mesh of a whole forest.

## The shape of it

1. **The mesh holds only what is big**, for the whole map: houses,
   fences, big rocks, cliffs. Everyone finds his way on it, near or far,
   so nobody "quantum tunnels" through a wall. Rectangles now, polygons
   maybe later (in the editor's bake, where time does not matter).
2. **The grid is thrown away once the rectangles are made.** In the
   editor when a map is baked; in the game only for the piece that
   changed (a house put down): "calloc, rectangles, free". On a map that
   was not baked: a tile's grid when the tile is first needed, then
   freed ("grila la cerere").
3. **Heights** come from the terrain; on an object (a terrace, a stair:
   rare) from a ray down into the physics. The rectangle says which.
4. **Small things are gone round locally**, not cut into the mesh:
   trunks, boulders, crates, buckets. Radu does not plan labyrinths of
   boulders and trees, and suboptimal paths are fine.
5. **Far from the player nobody looks at small things.** Past
   `navigation_detail_meters` (settings.ini, [simulation], 250 m) a
   walker goes straight along its path, through trees if it must; it is
   a few pixels. A path made far away gets its detours when the walker
   comes within the distance (each new segment is tested then).

## What a rectangle keeps

"Why the fuck do we keep so much?" What stays:

- its corners (quarter meters, within its tile);
- its height and what kind: **ground** (take it from the terrain),
  **flat** (the value), **uneven** (a mean; who is near and needs it
  exactly casts a ray down into the object: a ramp, an arched bridge, a
  big rock's sloping top). Each stair tread is its own flat rectangle;
- the ceiling's height, if it has one;
- the slope's cost (a byte; without it the bear climbs every hill
  straight);
- a flag "there are small things in here: keep your eyes open";
- where its neighbors start in the tile's list, and how many.

A neighbor is an index (16 bits; 8 are too few: the church's tile has
150 rectangles), and only **valid** neighbors are listed: the terrace
with a balustrade and the ground under it are not neighbors, although
the height between them could be jumped. That is decided when the tile
is built, on the grid, before the grid is thrown away. Neighbors in the
next tile: a list per side, as the border portals are now.

Not kept: the portal's two ends (two rectangles side by side touch along
the part of their sides they share: computed from their corners), what
kind of crossing it is (from the height between them and the actor:
within its step it walks, within its jump it jumps, else no way; ground
to ground the slope's rule), the search's scratch (below), the blockers'
groups (a door becomes a flag in a small list of doors of its own, its
state changes in the game), the water (read live at the search), "a
small object stands on it" (replaced by the flag above).

About 12-20 bytes a rectangle and 2 a neighbor, against 84 and 32.

## The search's scratch

A* over the rectangles: the cost so far, which rectangle it came from,
where it came in, whether it is done with, which search wrote that. It
is only good while one search runs and only for the rectangles that
search touched (a few, sometimes a few hundred), so it lives in a small
table of the navigation's, reused, not in every rectangle of the map.

## What the actor keeps

Its path as now (corners of the pulled string, at most 32), plus a byte
of flags per segment: "keep your eyes open here" (the segment goes
through a rectangle with small things in it), "jump" (as now). Room for
more (water to the knees, a door to open).

## Small things

Which objects are small:

1. a **module** (a house and its parts): a wall, as now;
2. an object on its own whose footprint is wider than a threshold: a
   wall. The footprint's longest reach on the ground, not its width: a
   fence's segment is thin but 2 m long, so it is a wall ("we are not
   stupid enough to build a fence out of single posts");
3. under the threshold and heavy: **gone round** (a trunk, a boulder);
4. under the threshold and light: **soft** (a bucket, a small crate):
   gone round at a walk, run through at a run (or kicked away by the
   physics); the feet do not climb on it (no surface from it), which is
   what twisted the bear before;
5. a key in objects.ini forces the kind, for the odd one.

The thresholds in the ini: 1.2 m and 20 kg to start with. The mass is
in the objects' data already.

What is kept of a small thing: **capsules in 3D** (two ends and a
radius, 28 bytes), fitted once per model from its collision triangles
(the pobj has the trunk alone as collision; no capsule is stored there:
the physics makes its own at load, and only for things that move), in
slices up its height for a crooked trunk; per tile, placed in the world.

When a path is planned and a segment has the flag: a loop over the
tile's capsules; each one cut to the band from the ground to the
actor's height and laid on the ground (a leaning trunk goes over a
hen's head half a meter from its root, not over a man's), swollen by
the actor's radius; does the segment cross it?

- **Step 1, the tangent:** a new point on the side with the shorter
  way round, checked to be in the mesh (else the other side), the two
  new segments tested again, a few rounds at most.
- **Step 2, only if step 1 fails in the game often enough:** an A* over
  a small grid made on the spot (12 m, quarter meters) from the swollen
  capsules and the mesh's edge, thrown away after. The machinery is the
  fine way's of today, fed from capsules instead of the map's grid.
- else it goes straight and makes do, or looks for another way.

No ray into the object's real triangles: the capsule is of the part in
a body's way already. If a cart's gap under it matters one day, a finer
test for those types then.

What moves (another bear, a rolling boulder) cannot be planned for: a
cylinder cast ahead a few times a second while walking, steering to the
free side; stuck all the same: the portal shut and another way, as now.
"It is a game, not a simulator of the universe": a bear clipping a
boulder that falls in front of it is fine.

## What has to move off the grid first

Today these read the grid while the game runs:

- the follower's guard, its body's line and the fine way (actors.c):
  replaced by "stay in the mesh (it is where the body fits) and go
  round the capsules";
- feet and body heights: the terrain, a ray down on objects;
- doors and blockers: a tile is built again when a door opens. A flag
  on the door's own entry instead, read at the search;
- water: a tile is built again when its water changes. Read live at the
  search instead (most maps will be baked without active water);
- jumps: the search checks on the quarter meters where it leaves the
  ground and lands. Decided at the build (a neighbor that is listed can
  be crossed); the spots follow from the corners;
- the table "cell -> rectangle": a search through the tile's rectangles
  (buckets).

## Order

Each step leaves main working and the three suites passing
(nav_stress.py run, houses.py --run, chase.py).

1. The new rectangle and neighbor, the search's scratch out of the
   rectangles, the table "cell -> rectangle" out.
2. Small things out of the mesh (the rule above), capsules per model.
3. Going round capsules at planning (the tangent), the flag per segment,
   `navigation_detail_meters`.
4. The follower without the grid: heights from the terrain and the
   physics, the guard replaced.
5. Doors, water and jumps without the grid; then the grid only while a
   tile is built.
6. The bake in the editor (a block per tile in the map's file, a hash,
   only changed tiles built again in the game).
7. The cylinder for what moves, other actors too (to check: have actors
   bodies in the physics?).

Later, if the numbers ask for it: polygons, islands (which rectangles
are joined at all), a coarse graph for ways of kilometers.

## Where it stands

- Steps 1 and 2 are in (Nov 19 2026). Step 2: `object_kind`
  (navigation.c) is the rule above; the reach is measured on the
  capsules up to 2 m (a man), the mass is the ini's or the density
  times the model's volume, and only bodies can be soft (a static thing
  is not kicked away). `fit_model_shape` makes the capsules (slices of
  0.5 m, a circle round each, one capsule if they line up, cut in two
  where they do not, 4 at most); a region keeps those of its small and
  soft things, in the world (32 bytes each: the type and "soft" with
  them). A rectangle's `small` says a capsule reaches into it. F4 draws
  the capsules.
- Step 3 is in (Nov 19 2026), and [simulation] navigation_small_out
  is on. `mesh_lead_round` (after the search, before the way is checked
  over the quarter meters): the corners of the way that lie in a thing's
  berth are pushed out of it; then, per stretch, the first circle
  crossed (the capsule cut to the agent's height, laid on the ground,
  a circle round it, swollen by the agent's radius + 2 cm) is gone
  round 20 cm further out, on the side with less of a turn, a point per
  quarter turn, each on the lines touching the circle; the new
  stretches must lie on the rectangles (else the other side, else the
  thing is left to the guard), and are looked at again. At most 64 new
  points; a way with no room for them (96 waypoints) is cut there, more
  after its last. `path_small` / `navigation_path_small`: the
  stretches by small things (no one reads it yet: step 4's guard).
- Not in the plan, needed: **`mesh_weld_small`**. A row of trunks 0.75 m
  apart was no wall to anyone, and no tangent goes round a thing whose
  neighbor's circle is where the point would be. When a tile is built,
  two small things with less than the class's width between their
  capsules take the quarters on the line between them out of the
  rectangles: a row is a wall, a gap wide enough is not. The grove of
  small.py: 4 rectangles for the hen, 10 for the farmer, about 50 for
  bear and cow (about 250 with everything a wall).
- `navigation_line_passable` says no where the line crosses a small
  thing (10 cm nearer than a way is let): a chase looks for a way.
- `navigation_detail_meters` (250): `navigation_set_detail` each frame
  with where the player is; further off the ways are not led round.
  Not done: looking at a far walker's way again when the player comes
  near (it is stopped by the guard and takes a fine way, as before).
- Cost (Radu: the game stuttered in a forest, the fan on): each line
  asked about and each look at a stretch went over all the capsules of
  its regions (thousands in a forest, many times a frame). Now a
  region's capsules are in the order of 4 m squares (`capsule_first`)
  and only the squares a line passes are read; a way's stretch gathers
  its things once (not again for each piece it is cut into); the weld
  is a sweep along x, not every pair. A chase through 1500 trunks:
  leading a way round 0.37 -> 0.08 ms, a search as a whole about as
  with everything a wall. The berth is 20 cm now (10: the walker's
  guard found no straight way by the trunk and took a fine way, 14
  times against 6; the chase 32 s late against 22, 21 as walls).
- More of the cost, found with a chase through 2500 trunks a meter
  apart (Radu's stutter: one search of a bear 14 m off, 1982
  rectangles, 6.5 ms): (1) a way with no room for its points round the
  trunks was told as partial: the walker searched again every dozen
  meters, a farmer's route was "no way". Now it is cut and "more after
  the last" (as a long way's corners always were), and a way holds 96
  points (32). (2) The check of a found way over the quarter meters
  (mesh_way_holds) looked for a fine way at every trunk the way passed
  (the body's line brushes them on the grid): most of a search's time.
  Not on stretches by small things now (path_small: crossed or within
  half a meter). (3) Flagging a tile's rectangles with small things
  tried every capsule against every rectangle (16 ms a tile there):
  through the tile's squares now. (4) The weld took the body's whole
  width between the circles: 0.9 of it (MESH_WELD_SHARE). (5) A point
  round a thing that fell in the next thing's berth (a gap narrower
  than two berths): moved to the middle of the gap. In that forest a
  search is 1.2 ms on average (1.0 with everything a wall); the tiles
  take 6 to 16 ms each either way (a thousand rectangles: as before).
- A region built again (a collapse, a tree felled) built the tiles of
  all eight regions round it again too, at the next search through
  them (mesh_margin_signature went by their generations): 6 to 16 ms a
  tile in a forest, a stutter of 50 ms. Now each region keeps a hash
  of its cells and small things along each edge (`edge_hash`,
  `edge_generation`), looked at again when its generation moves; the
  tiles round it are built again only if the edge toward them changed.
  The stutter line has "path tiles built" and "path build
  microseconds" (regions and tiles together) to tell building from
  searching.
- Soft things are not gone round at planning (the walker's own
  business as it goes: the height per cell, as before).
- tests/navigation/small.py passes: every goal, no body in a trunk or a
  boulder; bear running 94 s (96 with everything a wall), on the spot
  11 s (7).

## Known and left

- A memory regression of Nov 18: a cell partly under something gets a
  surface for its free ground, and a region takes its whole block of
  4096 surfaces (180 KB) at its first one, so every region with a tree
  takes it. It goes when the grid does (step 5).
- F4 in the game shows the mesh (rectangles colored by height, portals,
  edges to jump); the console's `navmesh` has the counts and the memory.
- Seen by Radu in the game, for later: a bear walking on the spot behind
  another bear that is in its way (step 7), and once a needless turn at
  a terrace's corner.
