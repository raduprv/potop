# What the water leaves on things

The objects' shaders (rocks, houses, props, the plants' bark and foliage)
know where the water stands and where it stood (`ground_water.glsl`; the
memory per 2 x 2 cells on the GPU, `water_marks.c`, `water_marks.comp`,
a few times a second while there is water, resting once it is long gone):

* **Under water**: darker and wet; the sunlight plays over it (caustics,
  fading with the depth); where a thing meets the surface a thin bright
  line wobbles with the ripples (not on foliage). No snow lies there.
* **The flood's mark**: what stood under water half a minute or so keeps,
  once the water goes, a film of mud up to where it stood a while (a passing
  wave leaves next to nothing), dark brown while wet, a pale dusty grey-beige
  once dry, a darker line at its edge, gone in some fifteen minutes of game
  time; foliage is only dulled by it. The mark belongs to the spot, not the
  thing: what floats, or was ever moved by the physics, takes none of it
  (only the water there now).
* **Drying**: what was under water stays wet (darker, shiny) for four
  minutes after.
* **Algae**: what stays under water grows a green-brown film over a quarter
  of an hour, in patches; out of the water it dries pale and fades.

What is whose: solid things take all of it, bark all but the shine,
foliage only the mud and the sunlight under water.

**Moss under water** on the rocks (the weathered props, `prop_body.glsl`):
where the algae would grow it grows instead (in patches, not under
overhangs; without the marks' memory, wherever the water is now), green
in the water, pale once dry. Painted on, but seen into: a few steps along
the view through its strands (parallax), which bump the light. Near the
eye it also stands up in shells (`prop_shell.vert`, `prop_shell.frag`):
the rock drawn again a few times, lifted a little off its surface each
time, its strands cut out of every layer, their tips swaying. Only the
props at their finest level of detail, within `moss_shell_distance_meters`;
a draw per layer for each of their models.

settings.ini `[render]`: `moss_shells = 8` (0: painted on alone),
`moss_shell_meters = 0.02` (how tall it stands),
`moss_shell_distance_meters = 12`.

settings.ini `[render]`: `water_marks = true` (false: no memory of the
flood, its texture let go and its pass not run; the water there now still
wets, lights and lines what it touches).
