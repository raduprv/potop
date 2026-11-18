# Navigation tests, and the open problem

For whoever picks this up next (Fable, a later Claude, Radu): what the
animals' navigation is, how to test it headless, where it stands, and
what is still wrong. Written by Opus 5.5 after the work in the commits
from 349cf6a (inertial turns) to 5717683 (the fine way).

## The problem, in Radu's words and in numbers

Animals (bears, cows, people) now get everywhere they should round the
church and the inns: up the stairs onto terraces and porches, along them,
round the houses, not through balustrades or walls, not into houses
whose doors are shut. **Eventually.** How they get there is bad:

1. **Spins at stairs.** Sometimes an animal turns 360 to 720 degrees on
   the spot before it gets out (worst when running, on a narrow terrace
   or at the top of a stair beside a post or the end of a balustrade).
2. **Turning on the spot looks wrong.** When an animal stops and turns
   (the step guard refused its step, see below), it turns slowly, with
   the walk animation slowed down, "like a stone in a kettle". There is
   no proper turn-in-place animation or pose for it.
3. **Two grids on top of each other.** The path is planned over the
   1 m navigation mesh. Following it, every step is checked at quarter
   meters, and when that check refuses, a second search over quarter
   meters (the fine way) takes over. It works, but it is a stack of
   fixes, and the two levels disagree (a path drawn over whole cells
   crosses a post inside a cell; the follower must notice and go round).
4. **A running cow** (3.3 m/s at 30 degrees a second: a turn 6 m wide)
   on a terrace 2 m deep can go round in circles for a minute.

The baseline (tests/navigation/houses.py --run, Nov 18 2026; seconds and
degrees from trace_analysis.py, a 0.25 s sample "on the spot" if it moved
under 0.1 m):

```
inn_2  bear run : all 7 goals, 48 s on the spot, the most 435 degrees, 3 spins
inn_2  cow run  : 237 s on the spot, the most 600 degrees, 6 spins, home goal missed
inn_1  bear run : 45 s on the spot, the most 407 degrees, 1 spin
church cow run  : 119 s on the spot, the most 240 degrees, goal 7 timed out
walking (all)   : 13 to 76 s on the spot, at most 210 degrees, no spins
```

What "good" looks like: the same goals reached; no spins (no run over
360 degrees on the spot); time on the spot a few seconds per walk, not a
minute; a running animal slows to a walk where its turn does not fit,
instead of circling; and, if the design allows, one level of planning
the follower can trust, not a path plus a guard plus a rescue search.

The decision is open: keep the pieces and smooth them (e.g. speed from
the path's curvature, a real turn-in-place), or plan at quarter meters
near buildings so the path itself keeps the body clear (a finer mesh or
corridor near objects, a corridor-based follower), or something else.
Talk to Radu before a big change; he wants the plan first.

## How navigation works now

Files: engine/navigation.c (rasterization, surfaces, the mesh, the
searches; ~6000 lines), engine/navigation.h (the API and long comments),
engine/actors.c (following a path: move_actor and the helpers above it).

1. **Rasterization** (rasterize_object, fill_solid, finish_region; per
   64 m region, at load and when a region is regenerated). Every
   object's triangles sampled into quarter meters (16 per 1 m cell) and
   bands of 0.25 m. Closed models are filled solid (underside to top,
   side to side along rows, up to 2 m). Tops of pieces chained within
   0.4 m become **surfaces** (navigation_surface_t, up to a few per
   cell): a height, and per quarter `below[]` (how far under the
   surface's height, or NONE, or BLOCKED where something stands at its
   height) and `room[]` (clearance above). Objects' tops within
   MERGE_METERS (0.1) of the ground count as ground; anything higher
   in a cell blocks the ground there (terrain_clearance per cell).
2. **The mesh** (mesh_build_tile, lazy per 64 m tile and per class).
   Three classes of agent by width in quarters (round(2r / 0.25)): 1 a
   hen, 2 a man, 3 a bear or a cow. A node per cell (ground) or per
   surface; usable for a class where a square of its width of quarters
   with room for its lowest agent fits (surface_square_room, looking
   into the 8 neighbor cells too). Usable cells merged greedily into
   rectangles (at most 16 cells a side) of alike nodes. **Portals**
   between rectangles where the quarters on either side meet within a
   step (node_edge_rise / node_edge_span), cut to the quarter lines
   that meet, dropped if narrower than the class.
3. **Search**: A* over rectangles (mesh_find_path), then the funnel
   (mesh_funnel, portals inset by the agent's radius). Lines
   (navigation_line_passable) walk the rectangles and their portals.
   The old cell grid is the fallback when the mesh cannot answer.
4. **Following** (actors.c move_actor):
   - inertial_turn: turn speed ramps up and down (0.2 s), at most the
     type's turn_degrees_per_second (bear 60, cow 30, farmer 90);
   - steer_point: a point 1.5 to 2.5 m ahead on the path, if the line
     to it is passable (rounds corners smoothly);
   - waypoints passed at 0.6 m (0.25 m on a straight walker), a corner
     cut short only where the line on is passable (corner_cut_passable);
   - speed scaled by cos(heading error), so it slows to turn;
   - **the step guard** (step_passable): each step's center line to its
     end plus a radius must be passable on the mesh, the quarter there
     free (navigation_quarter_free) and the body there not tight
     (navigation_quarter_tightness: of 8 points at 0.8 of its radius at
     most 2 on quarters it cannot stand on, or no tighter than now).
     Refused: it tries 30 and 60 degrees to either side, else stops
     (speed 0) and turns on the spot. This is where the spins come
     from: it turns toward the next point, the step is refused, it
     slides, turns again;
   - **the fine way** (navigation_fine_path): after 0.2 s of refused
     steps, or when stuck 3 s, an A* over quarters in a window round it
     and its waypoint (at most 12 m), quarters with room for its body,
     tight ones 4 times dearer; it follows those points closely;
   - placement: its height from what is under its middle within a step
     of what it stood on the frame before (actor_t.center_ground).

Agents' sizes come from their models (radius = half the model's width,
height = the model's height unless height_meters is set) and from
data/actors.ini (step_meters, maximum_slope, turn_degrees_per_second,
walk and run speeds). The bear is 0.35 radius and 1.0 tall, steps 0.4;
the cow 0.37 and 1.59, steps 0.3; the farmer 0.25 and 1.85, steps 0.4;
the hen 0.1, steps 0.2.

## The tests

Build first (see docs/claude/00_START_HERE.md for a fresh container):

```
export PKG_CONFIG_PATH=/home/claude/deps/sdl3-install/lib/pkgconfig
make -j8 && make tools
```

Headless runs use lavapipe and Xvfb (the scripts set VK_ICD_FILENAMES if
it is not set). Everything writes into build/.

- **nav_stress.py** — a course of 14 stations built from boxes (terraces
  with treads 0.15 to 0.45 m, balustrade openings 0.5 to 1.2 m, bar rails
  at 0.9 and 1.3 m, huts with doors 0.5 and 1.0 m wide and 1.4 and 2.0 m
  high, yards under beams at 1.3 and 2.1 m, a porch with posts). Each
  station's expectation per agent is computed from the agent's width,
  height and step, not written by hand ("?" where the size is on the
  edge of the quarter grid). Its trails are checked for going into the
  solid. Prints spins too.
  `tests/navigation/nav_stress.py build` (once: data/objects/nav_stress.pobj
  through tools/potop_convert; objects.ini has [nav_stress]), then
  `tests/navigation/nav_stress.py run [hen farmer bear cow bear+run cow+run]`
  (about 2 minutes for all). Now: 0 wrong.
- **houses.py** — the real houses (church, inn, inn_1, inn_2), bear, cow
  and farmer, walking and with --run running; goals reached, samples off
  the mesh, time on the spot, the biggest turn, spins. The goals some
  types must not reach are listed (the shut church; the church porch's
  beam at 1.65 m for the 1.85 m farmer; the inns' 0.32 m tread for the
  cow's 0.3 m step).
- **trace_analysis.py TRACE** — the same numbers for any trace, with the
  worst turns and where they happened.
- **plot_rects.py TRACE OUT.png X0 Z0 X1 Z1** — the mesh's rectangles,
  the trail and the goals as a picture (objects' tops colored by height).
  tools/animal_trace.py draws trails over the obstacles too.

The game's own switches (game/main.c): `--houses NAMES` (the gallery:
houses alone on flat ground, the first at about (96, 110), logged as
`Houses: 'name' at (x, z)`), `--sim SECONDS` (headless, ends early when
the walk test is done), `--trace FILE`, `--walk_test TYPE "x,z;x,z"`
(up to 64 goals; an actor named "walk test" so no game mind takes it;
logs each path and `Walk test: goal N (...): walk_test_there/short/timeout
D, at X Z, Y m up`), `--walk_from x,z`, `--walk_run`. With paths logged
(the walk test turns it on) an actor stuck 3 s logs where and why, and
each fine way is logged.

## Rules of the house

- C11, Allman braces, tabs, long snake_case names, no function-like
  macros, English comments in the style of the code around them.
- Radu does performance and visual checks himself; ask before long runs.
- Do not touch textures. data/actors.ini and data/objects.ini may change.
- Every navigation change: run nav_stress.py and houses.py before and
  after; a new failure mode gets a station in the course.
