# What the water does to things

The objects' shaders (rocks, houses, props, the plants' bark and foliage)
know where the water stands now (`ground_water.glsl`: the surface between
the four cells round a point, from the ground and water textures). Nothing
is remembered.

* **Under water**: darker and wet (not foliage; bark less); the sunlight
  plays over it (caustics, fading with the depth). No snow lies there.
* **Moss**: the rocks grow moss on their part under water (prop_body.glsl,
  `water_moss_share`; standing strands near the camera with the moss
  shells: Water tab, Moss shells / Moss height).

Taken out in 39s (Radu: they looked bad, and cost every pixel of every
thing, a kilometer from any water): the flood's mud mark and its line, the
drying after, the algae's memory, the bright line where a thing meets the
surface; with them `water_marks.c/.h`, `water_marks.comp`, the
render.water_marks setting ("Flood marks") and the marks texture's sampler
(the objects' fragment shaders have 13 samplers now, lights at 13).
