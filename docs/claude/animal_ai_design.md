# Animal AI redesign (discussed 2026-11-15; the boredom walk coded in 39j
for the cows: game/wander.c, see 01_engine_map.md)

Radu's design. Do NOT code before he says so. The discussion so far is
ONLY about the boredom walk; the other activities come later.

## Structure

- One AI per animal: game/cow.c, pig.c, sheep.c, hen.c (like bear.c);
  the blacksmith too, with his own constraints. Not one mind with a kind
  table (animals.c's flags grew into an if-forest).
- Shared tools only (no behavior) in something like animal_common.c:
  spots, areas, the way into the byre/fold and back, stall choice, the
  "is it free" check. The night routine (in, down, up, out) shared so a
  bug is fixed once; each animal decides when and where.
- Order proposed: the cow first, Radu tests, then the others one by one.

## Tick

- Every 100 ms all actors walked through (staggered by id, so not all in
  one frame: no spike every 100 ms).
- Each has needs: hunger, thirst, tiredness, boredom (more later) and a
  current state. The needs pick the activity, with hysteresis (no
  ping-pong between feeder and trough at a threshold: an activity is kept
  till its need drops under a lower mark or it is done).

## Boredom walk (cow, bear, blacksmith alike)

- On its tick, in walk mode, it looks ahead (at least 1 m, per animal).
- Free ahead: keeps straight on, with a 10% chance (tunable) per tick to
  change heading by ±10°, only if that way is free too.
- Not free ahead: stops, or tries other headings at ±10°. None free: a
  step back if free behind. Not even that: keeps trying for about a
  second, then idle.
- Head-on into a fence it may do 2-3 steps to and fro before the drift
  frees it: accepted, real cows do that.
- Only this boredom walk works so. Walks with a purpose (water, feeder,
  byre) keep a computed way, but start on an arc, not a spin in place.

## "Free"

- Only what is physical stops it: fence, another animal, feeder, trough,
  walls, anything that would block it. NO navigation grid.
- A sweep as wide as the body along the heading, as long as the lookahead
  (a thin ray slips between two fence posts). Other actors as circles or
  capsules, moving ones also where they will be in about 0.5 s.
- Deep water and too steep slopes probably obstacles too (to confirm).
- Yard areas do not limit the walk (only the physical fence). Yards stay
  for other things (where the grass is).

## Wild animals

- No circle of ground, ever.
- Avoidance areas (e.g. not near houses): SOFT (Radu: hard would let you
  attack the bear, walk toward the village, it leaves, attack again and so
  on). They weigh only the boredom walk's heading choice.
- Terrain preferences: goat on the mountain, deer in the forest. Folded
  into the heading choice: each of straight / +10 / -10 scored a little
  ahead, the better one picked more often.
- Forest etc.: marked zones (by the editor or at load time). NEVER a loop
  over all the objects, least of all for every actor every 100 ms. Don't
  ask Radu what happens when the forest is cut down.
- Open question: territory. Claude suggested a "homesick" need growing
  with the distance from its den (the bear back to sleep there), so it
  does not wander off the map for life. Not answered yet.

## Still to discuss

- 4: the herd: NOT needed now (Radu: the yards are tiny, they are never
  alone in them). Maybe later for open pasture or wild herds.
- 5: DECIDED: a heading and speed given to actors.c straight ("a body in
  motion stays in motion"): it keeps going that way at that speed, its
  turn rate limited, till the mind says otherwise. No point each tick
  (no slowing down to "arrive" every 100 ms).
- 6: staggering (agreed in principle).
- 7: hysteresis (agreed in principle).

## Trace picture (asked 2026-11-15)

tools/animal_trace.py already draws the tracks into a png; extend it so
Claude can look at it (vision) after each --sim run. Each step coloured:
green walking free; yellow the 10% drift turn; orange blocked ahead and
turned to another heading; red forced: the step back, a turn wider than
the limit, walking in place; grey idle after giving up. Needs a trace
event from the new walk for each of these.

## Done in 39j, and what is left

- Done: the boredom walk (wander.c, shared, for any animal), the heading
  and speed mode, the physical free check, the coloured trace. Cows use it.
- Deviations: a walk whose FIRST look is blocked does not back up (no walk
  at all: back-on-back walk after walk otherwise); at most 2 steps back a
  walk; legs going but body stuck 1.5 s counts as blocked.
- Not done yet: the split into cow.c / pig.c / sheep.c / hen.c (cows still
  in animals.c, only their walk is new); needs (hunger, thirst...);
  hens, pigs, sheep on the new walk.
