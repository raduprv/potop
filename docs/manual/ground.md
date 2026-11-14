# How the ground reacts

Where it is drawn from: the ground the simulation holds on the GPU. Each
frame `water_ground.comp` makes the terrain's textures (heights, normals,
surface: silt, bare rock, water on it) for the tiles whose ground or water
changed, there, without the CPU (`render.terrain_from_gpu`, Graphics →
Water → Terrain from the GPU; off, the CPU makes them from its copy, as
before, to compare). The silt and the bare rock shown follow the ground in
a second and a half, so a film of silt laid and washed away again and
again under running water does not flicker. The CPU's copy of the ground
and of the water (for the physics, the objects standing on the ground, the
paths, the shadows' redraws, the ground's moisture) comes back a tile at a
time, at most every quarter second (the ground) or tenth of a second (the
water; a twentieth near the camera); the moisture stays on the CPU and goes
up as a byte a cell for the surface's wetness.

Each cell is a column: **bedrock** at the bottom (never erodes, holds no
water), the cell's **soil** on top of it (deep in flat valleys, thin on steep
slopes), and **silt** on top, which is whatever the water dropped there.

* **Moisture.** Ground under water or in the rain gets wet until the soil
  column is saturated (sand fast, clay slowly); where nothing wets it, it
  dries out again. Wet ground looks darker. The water itself stays: the
  moisture is a number for the farms and the look, not water taken away.
* **Erosion.** Flow faster than a soil's critical speed picks material up,
  until the water carries as much as its speed and depth allow. Under
  flowing water the ground is soaked, so it gives way at the wet critical
  speed. Silt goes first, then the soil, never the
  bedrock; soil worn down to the bedrock shows bare rock. Water carrying
  sediment turns brown.
* **Trickles do not dig.** Erosion power grows with the depth: nothing below
  half of `erosion_minimum_depth_millimeters` (20 mm), full from it up. A
  lake seeping over its lip does not cut itself an outlet; one really
  overflowing does.
* **Deposition.** Water carrying more than it can (because it slowed down or
  ran thin) drops the excess as silt.
* **Bank collapse.** Banks with water at their foot (or on them) steeper
  than the soil's wet slope slide into the water. This is what really breaks a dam: the overflow cuts a notch, the
  sides of the notch slump in, the notch gets wider and deeper, more water
  goes through. Silt (mud the water dropped) is loose: it slumps to its
  slope even when dry.

* **Smoothing.** Under water, a single cell sticking out above (or dipping
  below) both its neighbors on a line, the sawtooth erosion carves under
  cliffs, is evened out with them (`erosion_smoothing` in `soils.ini`). Only
  loose material moves, never bedrock.

All of it only runs where water flows or carries sediment. The fine shadow
map around the camera's target is redrawn when the ground in it has changed
by `shadow_erosion_meters`; the far map ignores erosion (it does not show
centimeters), only tools and stamps redraw it. Objects carried by the water
only redraw the fine map; in the far one a log is a pixel of a soft shadow.
Objects standing on the ground are set 2 cm into it, so erosion does not
uncover their feet; once the ground under a chunk wore away more than that,
its objects are put back on it (a few thousand a frame).
