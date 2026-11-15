# Animal AI redesign (discussed 2026-11-15, nothing coded yet)

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

- 4: the herd (a "lonely" need biasing the heading choice toward the
  group: big for sheep, small for cows, none for the bear).
- 5: how the engine takes "walk this way" (a point 1-2 m ahead each tick,
  or a heading and speed given to actors.c straight).
- 6: staggering (agreed in principle).
- 7: hysteresis (agreed in principle).
