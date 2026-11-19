# Tuning

* `data/settings.ini`: terrain generation, terrain cache, camera, rendering,
  shadows, water look and physics, tools, simulation rate, objects (populate
  new maps, density and draw distance scales), object physics (`[physics]`). Removing a key (or
  the whole file) restores the default compiled into `engine/settings.c`; the
  meaning of every value is commented in `engine/settings.h`.
* `data/soils.ini`: what each soil does (sand, earth, clay, rock and silt, the
  mud the water leaves behind): how fast it gets wet and how much water it
  holds, bed friction, the flow speed it starts eroding at, how fast it
  erodes, and the steepest slope it holds under water. Also transport and
  deposition rates and the generated soil depth.
* `data/objects.ini`: object types (see Objects below): what each model is,
  how big, how far it is drawn, its shadows, its physics (body, anchor,
  floating, flood behavior, damage), where it grows and how densely.
* `data/layouts.ini`: layouts (see Layouts below): which objects make a
  homestead, a camp, a hamlet, and where new maps get them.
* `data/materials.ini`: terrain textures. For a texture named `grass_01` the
  game loads `data/textures/grass_01.png` and, if present, its normal map
  `data/textures/grass_01_normal.png` (green up the picture, OpenGL's;
  `normal_green_down = 1` for DirectX's); without one the normals come from
  `data/textures/grass_01_height.png` (the color + height pair texgen
  exports), or else from the brightness. Up to three variants per
  material. Where two materials meet each breaks into patches of the other,
  not a blend by height: `patch_meters` (how big its patches), `patch_edge`
  (how soft their edges, meters; 0.03, at least a pixel and a half: far off
  it turns into a plain blend), `patch_strength` (how far they break the
  border; 0: a plain blend). The noise is in the shader, in world space:
  nothing repeats, and only the materials that show in a pixel are read.
  `wet_texture`: what it looks like soaked (clay: `wet_texture = mud_01`,
  made by `potop_texture mud` with the clay's seed). Where the ground is
  under water (at once) or soaked (clay soaks slowly) it shows that
  instead, in patches as it dries, the edges first: the clay's cracks close
  under the water and open again as it dries. The silt the water leaves can
  have one too (`[silt] wet_texture = mud_01`). Roughness (how matte, 0 a
  mirror .. 1 chalk): `grass_01_roughness.png` (gray; or the green of
  `grass_01_arm.png`), else the material's `roughness` all over (0.9; rock
  0.75, snow 0.6), its wet texture's `wet_roughness` (0.35). The sun's
  highlight follows it (GGX), and smooth ground (roughness under 0.5: wet,
  under water) mirrors the sky a little at grazing angles; wet ground is smoother still (the water in its pores), and
  under standing water nearly a mirror. The layers are made once (decoded,
  normals and roughness put in, mipmapped) and kept compressed (BC7) in
  `data/cache/textures`, as are the models' textures: next time they go up
  as they are. A CRC32 of everything they were made from (materials.ini,
  every source file's bytes, `texture_size`, `normal_strength`) decides
  whether they are made again; a texture with a `_normal` file has its
  `_height` file left unread. Delete the folder to make them all again.
  `tiling_meters` is the size of one repeat up close,
  `macro_tiling_meters` the size at a distance (the shader fades from one to
  the other; if omitted it is ten times `tiling_meters`). Without any texture files every material uses a procedural
  fallback, so the game runs with an empty `textures/` folder. Silt has its
  own material, `[silt]`.
* `data/shaders/`: GLSL, compiled to SPIR-V by `make shaders`
  (`data/shaders/spirv/`), loaded at startup: after editing one, `make
  shaders` is enough, the game itself need not be rebuilt.

For texgen textures, export with `light_strength = 0`: the game lights the
relief itself from the normals, and baked lighting would fight the real sun.

The look of the ground is tuned from `[render]` in `settings.ini`: blend
width between soil types (`soil_blend_meters`), slope ranges for grass and
bare rock, a global variation texture over all materials (load your own as
`data/textures/variation.png`, grayscale and tileable, or let the game
generate one) and moving cloud shadows (coverage, wind, height). The same
variation texture is used for the clouds.

Rendering speed, also in `[render]`:

* `lod_distance_meters`: terrain level of detail. Chunks farther than this
  are drawn with every second vertex, twice as far with every fourth, and so
  on (down to every 32nd). Vertices slide onto the coarser surface before a
  chunk switches, so nothing pops, and lighting uses per pixel normals, so
  far terrain keeps its relief detail. Raise it if distant ridges look too
  smooth; 0 draws everything at full detail. It is never set lower than 2.5
  chunk diagonals (about 57 m at 0.25 m cells), below which seams could open.
* `far_terrain_upload_seconds`: ground that changed beyond full detail or out
  of view (erosion under far water changes it every step) is sent to the GPU
  at most this often; up close and in view it goes at once, and a chunk that
  comes close or into view is sent right away. 0 sends everything at once.
* `far_water_upload_seconds`: the same for the water (0.2 s). Moving water
  is sent to the GPU every step otherwise, 12 bytes a cell: the depth as a
  full float (the surface is ground + depth, and half floats would put
  centimeter steps on deep lakes), velocity, mud and the F6 speed as half
  floats.
* `render_scale`: the 3D scene is drawn at this fraction of the window
  resolution and scaled up. 0.75 shades about half as many pixels; on a big
  screen that is most of the terrain's cost.

**Day and night** (Graphics → Day and night, `day_cycle`, on unless set).
The sun crosses the sky once a day (`day_length_minutes`, 240 real
minutes; 0 keeps the hour). It is up `daylight_hours` of the 24 (14, about
noon: up at 5:00, down at 19:00): it rises a quarter turn before its noon
direction, stands at Sun height at noon and sets a quarter turn after it.
An hour of dusk takes it 12 degrees under the horizon, the night (8 hours)
as deep as its noon height, an hour of dawn brings it back (the sun
settings are noon's). Low, its light turns
orange and fades; at dawn and dusk the sky turns a dusky blue, with a warm
glow low on the side where the sun rises or sets (only there, fading round
to the other side and upwards), then goes dark blue; the stars come out (a few in a hundred tiny cells of the sky,
most faint, turning about the pole once a day, twinkling, hidden by the
clouds, the volumetric ones too, looked up in their dome) and the moon
lights the land faint and blue and casts the shadows (the light swaps from
the sun to the moon while both are too faint to see).

**The moon** goes round its phases in `moon_cycle_days` (29.5 days of the
game, as the real one; every map starts a day before full moon). It keeps
the sun's path, as far behind it as its phase: new, it goes with the sun
(up by day, unseen); at first quarter it is up from noon to midnight (in
the afternoon sky too, pale); full, all night; at last quarter from
midnight to noon. Its light goes with how much of it is lit (squared: a
quarter moon gives a quarter of a full one's) and with its height; when
it is down, or new, the stars give a little light from overhead, and the
night is dark but for the fires and the lanterns. On the sky it is a ball
`moon_size_degrees` wide (1.5; the real one is half a degree) with the
real moon's map on it (`data/textures/moon.png`: NASA's LRO color in rgb,
its heights in alpha, 512 x 256, public domain), lit from the side its
phase says (Lommel-Seeliger, as the dust lights: the full moon a flat
disc), the craters along the line between day and night shaded from the
heights, the dark part faintly lit by the earth near new moon; behind the
clouds, reddened low down, and the stars do not shine through it.
`moon` in the console tells its age, phase and place, `moon 0` makes it
new, `moon 14.75` full. The water mirrors it too (a plain gray ball there: the ripples
smear it anyway), where the mirror finds no land on the screen.

**Valley fog** (Sky & light, Valley fog; `engine/fog.c`): low milky fog in
the valleys and over the water, its top soft and torn into wisps that
drift slowly with the wind; it forms in the evening (`fog_form_hour`, 19,
thick four hours later), lies through the night and the dawn, and lifts
in the morning (gone by `fog_clear_hour`, 10; `fog_always` for looking at
it). Where it lies: a valley map made from the terrain every few seconds
(each place against the ground smoothed over `fog_valley_reach_meters`,
250: the deeper it lies below, the higher its fog, up to
`fog_valley_height_meters`, 30; over water two thirds of that), its top
smoothed to lie level as a lake's. `fog_mode`: 2 volumetric (the view cut
into froxels, `fog_quality` 0..2: 96 x 54 x 64 to 240 x 135 x 128 cells, each frame a little elsewhere in them and built on the frames before, the same places in the world, the
fog and its light in each, the sun's through the shadow maps, so ridges
and trees throw their shadows into it, and the sky's; gathered from the
eye outwards; `fog_inject.comp`, `fog_integrate.comp`, `fog_apply.frag`),
1 simple (each pixel marches its ray in 16 steps, no shadows:
`fog_simple.frag`, for weaker GPUs), 0 off. `fog_valley_density` (0.08 a
meter), `fog_softness_meters` (its top), `fog_glow` (brighter towards the
sun, 0..0.9), `fog_night_brightness` (0.3: by night its light is that
much of its full; lit evenly from every side it would outshine the dark
ground round it) and `fog_distance_meters` (600) do the rest. Drawn once the
water is in; the particles and the clouds come over it.

**Fireflies** (World window, `fireflies`, 0.5): on dark nights, not in the
rain, swarms of them about the eye (on foot) or where the camera looks:
on earth or clay (where grass grows), not on rock, sand or water, most of
all within a few meters of water. Each swarm (`engine/fireflies.c`) is the
`firefly` effect of `particles.ini` going on for half a minute or so, then
another somewhere else; the fireflies are small and drift a long while,
flashing in bursts all together, as the synchronous kinds do (`flash` in
the effect: six flashes half a second apart, then dark, every 7 seconds;
most kinds flash each on its own: `flash = ..., 0`). At dawn
they go, one swarm after another. The clouds take the light of the hour; at night their light from
the sky drops to a fifth, so they stand dark against the stars. At night the
exposure is set 2.2 stops darker, so the night stays a night. The shadows
are drawn again each time the light has moved a fifth of a degree. In the console, `hour` tells the time and `hour
18.5` sets it (every map starts at 10:00; the hour is not saved), `freeze`
stops the day and lets it go again. The Sky window has the same at its
top: an Hour slider and Stop. See `engine/daylight.c`.

The scene is drawn as light, not as a picture: linear, in half floats,
where the sun glinting on the water may be fifty times brighter than white.
The frame's last pass (`engine/post.c`, Graphics → Light (HDR), `hdr`, on
unless set) makes the picture of it:

* **Exposure.** The scene averaged down to a few dozen texels, the mean of
  the logarithm of its brightness (the middle of the screen counting more),
  brought to middle gray, the way the eye adjusts. It follows slowly
  (`exposure_adapt_seconds`, 1.5 s): from a dark valley up into the sun the
  picture is too bright for a moment, then settles. `exposure_ev` moves it
  by stops; with `auto_exposure` off, it is the exposure alone.
* **Bloom.** The scene halved six times with Jimenez's thirteen tap filter
  (the first time weighing bright groups down, so one burning pixel does
  not blink), then summed back up with a tent at each size: the picture
  blurred at every size at once, a little of it (`bloom_strength`, 0.05)
  mixed into the scene. Everything glows a bit; what is far brighter than
  white glows a lot.
* **Tone mapping** (`tone_mapper`): any light into what the screen shows,
  without cutting it at white. AgX (0) turns bright colors towards white
  instead of flat yellow or cyan; AgX punchy (1, the default) is the same
  with more contrast and color; ACES (2) is the film look, saturated;
  Reinhard (3) the plainest.
* **Night vision** (`night_vision`, 0.7; `night_vision_luminance`, 0.05):
  where the scene's own light (before the exposure) is under that, colors
  fade towards a pale blue, all the way at a tenth of it, as the eye's rods
  take over from its cones (the Purkinje shift). A moonlit field turns
  blue-grey; the ground by a fire keeps its colors.

**Local lights** (Graphics → Local lights, `local_lights`, on;
`local_light_intensity` scales them all): fires, torches, lanterns and
candles light what is around them. A particle effect gives light with
`light = r, g, b` in `particles.ini` (the campfire, sparks, the magic
ones): it comes up as the effect starts and a burst's dies with its
particles. An object type does with the same keys in `objects.ini` (the
torch, the lanterns, the candles), from near the top of its model unless
`light_height` says where. `light_intensity` (1: about the noon sun on what
is a meter away), `light_radius` (meters; nothing beyond), `light_flicker`
(0 steady .. 1 a dancing flame: its brightness and, a little, where it is).
They light the ground, the objects, the actors and the water; none casts a
shadow. By day the sun drowns them: they fade out as it rises above the
horizon (gone, and costing nothing, from 8 degrees up) and in as it sets;
flames and cores still show. Lamps (`light_switched`, true for objects
unless set: lanterns, candles; the torch burns on) are lit at dusk and put
out at dawn, each at its own moment (`lamps_with_the_day`, on), or all at
once by the console (`lights on|off|auto`) or the game (`lights_set_lamps`).
`light_core` (meters) puts a small glow where the light is, a candle's
flame or a lantern's glass, drawn facing the eye and halo'd by the bloom:
no glowing textures needed, the lantern's frame stays dark.
`light_height` below 0 is that share of the model's height (-0.6, the
default: about a lantern's glass). The **Light** tool (Debug) places lights
(lamp, fire, cold, white; brightness, reach and height) and a click next to
one removes it; they are kept with the map (and the continuous effects
the Effect tool plays, where they are; on an actor they are not). `engine/lights.c` gathers each frame the 512 nearest to the eye (out
to 300 m) and sorts them into clusters: the screen in 16 x 9 tiles, each
cut in 24 slices by the distance from the eye (closer slices thinner); a
pixel (`data/shaders/lights.glsl`) looks only at the lights of its own
cluster, so hundreds cost about what a few do.

Then gamma, and half a step of 8 bit noise against bands in the sky.
Off, the light is cut at white, as the game used to show it (but the water
and the clouds now blend in linear light). The pass also stretches the
scene over the window (`render_scale`).

F4 logs how long the GPU spends per frame on the shadow map, terrain, water
and the upscaling, and how many terrain chunks were drawn at each level of
detail.

Generated terrains are cached in the user's preference directory
(`~/.local/share/potop/cache/` on Linux) and reused when the same seed
and settings come up again; only the newest few files are kept.
