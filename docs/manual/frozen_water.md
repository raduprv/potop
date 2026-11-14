# Frozen water

Once the world looks right, the water can stop: F7, or by itself after
`freeze_after_seconds` (30 s) without the player touching terrain or water.
Then nothing is simulated: no water, no erosion, no soaking or evaporation,
and springs stop pouring. The water keeps the flow it had, so it is still
drawn flowing (the ripples and foam drift with the current) and floating
things still drift on it; the physics keeps running. Any tool that changes
terrain or water, a spring, rain, a new or loaded map, or F7 again starts
the water where it left off. Things falling into the water do not: only the
player does. The whole map stops and starts at once, so there are no seams
between frozen and running water. 0 for `freeze_after_seconds` freezes only
with F7. Space pauses everything, the physics too.

Water thinner than about a millimeter clings to the ground as a film: it does
not flow and slowly evaporates, so the map dries out after rain instead of
trickling forever. A fake surface tension does the same for full ponds: water
only spills onto dry ground when it stands a few millimeters above it, so a
pond filled to the brim stops draining over its lip instead of leaking slower
and slower forever. The lip (`lip_millimeters`, 20 mm) does the same where the
water below the lip is already wet: deep water spills into thin water only when
it stands that much above it, so a pond a little over its lip stops instead of
draining for hours through a trickle.

This is a game, not a hydrology tool, so **thin water** (up to
`thin_water_millimeters`, 3 cm) is helped along, each part with its own switch
in `[water]`:

* **Superfluid** (`thin_water_superfluid`): thin water flowing on over wet
  ground flows as if it were 3 cm deep, without the extra friction real
  shallow water has, so a trickle takes seconds instead of half an hour. Into
  dry ground it stays real, so a flood's front is the real, thick front and
  not a film racing ahead. Its speed (for erosion, the physics and sleeping)
  is measured as if it were 3 cm deep too, or a fast film would hit like
  rapids.
* **Drying** (`thin_water_evaporation_millimeters_per_second`): thin water
  left behind in still tiles, with no deeper water next to it, evaporates
  fast (not at lake shores).
* **Mud settling** (`still_water_speed_meters_per_second`,
  `still_water_deposition_per_second`): mud in water slower than that drops
  out within a second or so instead of keeping its tiles busy for minutes.

Lakes come to rest for good thanks to two more rules:

* **Static friction** (`still_level_millimeters`, 5 mm): water that just came
  into a cell through a pipe does not go back out through it for less level
  difference than that. A lake's rocking is water going back and forth, so
  it stops once the waves are smaller; flow in one direction is never held,
  so tilted water still levels out and rivers on gentle slopes flow.
* **Shore damping** (`shore_damping_depth_meters`, `shore_damping_per_second`):
  slow water gets braked harder the shallower it is near the shore, so waves
  die on the beach instead of bouncing back. Fast water is not touched.

Bodies ignore water shallower than `water_minimum_meters` (3 cm, in
`[physics]`), feel the water surface interpolated between cells, never see a
flow faster than 10 m/s, and floating ones have their bobbing braked
(`float_damping_per_second`).

The status line shows where the time goes, in milliseconds per frame: the
whole frame, drawing (CPU side, and the graphics card when it can be
measured), water, erosion and physics. Next to the average fps it shows the
minimum (the slowest frame of the last half second), since an average hides
stutters. Any frame much longer than the usual one is logged as a
`Stutter:` line with where its time went (update, poll, water, erosion,
wetting, physics, particles, water end, copy wait, acquire, navigation,
terrain upload, shadows, render, present) and what happened in it (steps,
substeps, evaporations, bank collapses, tiles copied back, chunk uploads,
physics rebuilds, shadow redraws). "acquire" is waiting for the screen
(vsync) or for a busy graphics card. Vertical sync applies at once, without
Apply.

The surface mirrors what is around it (Graphics → Water → Reflections,
`water_reflections`, on unless set). Before the water is drawn, the scene so
far (its color and its depth) is copied, and each reflected ray, bent by the
ripples, is marched over that copy in steps that grow as it goes (screen
space reflections): the shore, the hills, the trees and the people
standing by show upside down in it. Where the ray leaves the screen, goes
behind the eye or finds nothing, the sky takes over, with the volumetric
clouds looked up in their dome (the same one the sky shows, from the
eye), so the clouds drift over the lakes too. The mirror fades into the sky
near the screen's edges and at the end of the march's reach, and it is
strongest at grazing angles (Fresnel): looking straight down at the water
there is almost nothing to mirror, and the march is skipped. Off, the water
mirrors the sky alone, as before. The march follows only a third of the
ripples' bend (with all of it, at the water's level neighboring rays go
every which way and the shore breaks into stripes); a ray the ripples
bend down into the water bounces up again, as it would off the next wave;
and a hit counts only where the ray went through a surface in that step,
not behind a blade of grass. What is not on screen cannot be mirrored:
a tree behind the camera, or the far side of a hill. See
`data/shaders/water.frag`.

On request the marching is done for a quarter of the pixels
(`water_reflections_half_size`, Graphics → Water → Reflections at half
size): the water is drawn with the sky in the reflection's place, writing
beside its color each pixel's reflected ray and how much of the pixel its
reflection is; a compute pass marches, for each 2 x 2 pixels, the one that
mirrors the most; a last pass adds what was found in the sky's place, each
pixel taking the nearest marched texels whose distance matches its own (a
far lake's reflection does not bleed onto the near shore). Faster, a little
softer (off by default: the gain is small and the softness shows); off,
every pixel marches its own. The march itself: 32 steps
growing by 16% each, then 4 halvings to the surface.

Under water, the camera sees the world the way a diver does: everything
fades into the water's color with distance (shallow color near the surface,
deep color and darker further down, murky brown in muddy water), sunlight
focused by the ripples plays over the ground (caustics), and the surface
seen from below shows the sky only straight above (Snell's window, wobbling
with the ripples) and mirrors the water elsewhere. How far you see in clear
water (`underwater_visibility_meters`, mud cuts it down a lot) and how bright
the water around the camera is (`underwater_brightness`) are in Graphics →
Water. See `data/shaders/underwater.glsl`.
