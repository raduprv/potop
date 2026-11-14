# Sky

The sky is a dome at infinity, worked out for every pixel
(`data/shaders/sky.glsl`): `sky_color` at the horizon (the same color the
distant land fades into, so there is no seam), `sky_zenith_color` straight
up, the sun's disc and glow, and the clouds. The clouds are the same moving
sheet whose shadows cross the land: a ray from the eye is traced up to it
(the sheet is curved like a small planet's sky, so it comes down to the
horizon and fades into its haze),
so the cloud overhead is the one shading the hill below (three turned layers
of the noise texture, so its repeats never line up into a grid towards the
horizon; `data/shaders/clouds.glsl`, shared with the shadows). Seen from high
up the land ends below eye level, so the sky's horizon is lowered to where
the haze swallows the land. The water mirrors
the same sky (with blurrier clouds: the ripples scatter its rays). Under
water there is no sky, only the water's color.

With `volumetric_clouds` on (Graphics → Clouds → Volumetric, the default)
the sky leaves out its flat sheet and real 3D clouds are drawn over the
scene instead (`clouds.c`, `data/shaders/clouds_*`): a layer from
`cloud_height_meters` (at least 400 m, well above every mountain) up
`cloud_thickness_meters`, with `cloud_steps` setting the step inside clouds
(clear air is crossed in steps three times longer; far away all steps
grow). Where the clouds are is still the flat cloud field, so the shadows
on the land lie under them; their shape is a 3D noise of broad billows
eaten at the edges by a finer one, computed once (then kept in the
preference directory's cache). The light: absorption towards the eye and
towards the sun (four steps), dark edges facing away from the sun, silver
linings around it (held back so they do not burn white), the sky's light
from above. The layer is a shell around a planet of 200 km radius
(`CLOUD_PLANET_RADIUS`, `common.glsl`), so the clouds come down to the
horizon and fade into its haze, as in the real sky: a layer at 1000 m meets
the horizon about 20 km away (the Earth's 6371 km would put it at 110 km,
too far to march; 40 km, as it was, at 9 km, the clouds heaped up there).
They are marched up to 80 km away. Clouds → Horizon drop
(`cloud_horizon_drop_meters`, 1000 m) lets the planet's surface down under
the eye while the layers stay where they are over it: the rays a little
under the horizon still reach the clouds, so they go on a few degrees under
its line (about sqrt(2 drop / radius): 6 degrees at 1000 m) instead of
stopping at it.

The haze (Graphics → Haze, `fog_density`) is densest at height 0 and thins
upwards, e times every Haze height (`fog_height_meters`, 1000 m): mountain
tops and clouds are seen through less of it. For the land it goes along
the straight way to the eye; for the clouds along the ray over the curved
ground, which stays low for a long way towards the horizon. Hidden clouds
fade out to the sky behind them (hazy at the horizon itself) instead of
being painted the haze's color, so they never stand out as patches of
another blue.

Clouds → Type picks a kind of clouds, which sets the values
below it (`cloud_type` in the ini: 1 small fair weather puffs, 2 medium
heaps, the default, 3 storm towers with dark bases, 4 overcast; changing
any of the values makes it 0, custom): the cover's coverage, softness and
spread (`cloud_tiling_meters`), the layer's height, thickness and density,
the billows' size (`cloud_size`), how tall the thick parts grow
(`cloud_towers`) and how dark a thick layer's bottom gets
(`cloud_base_darkness`). Inside the clouds the light is scattered many
times (three octaves of ever fainter, ever more even light), so thick
clouds stay bright inside instead of turning black.

The clouds' shapes come from an atlas made at start (`cloud_weather.c`):
thirty shapes grown out of blobs the way real clouds heap up (lumps, smaller
lumps bulging from them, and smaller still)
(fair weather puffs, cauliflowers, towers, storm clouds with an anvil
downwind, flat sheets), each kept as a small 3D distance field. They are
stamped over a map of the sky 25 km a side (it repeats, and the wind
carries it), turned and scaled at random, as many as the coverage says, as
far apart as the spread, as tall as the towers allow; the shader only
roughens their surfaces into billows. Their shadows on the land are their
own: twice a second a compute pass looks up through the layer over every
patch of the cloud map (512 a side, about 50 m each) and keeps how much of
the sun gets through; the land, the water, the objects and the people read
it where their way to the sun meets the clouds' base. Without the
volumetric clouds the flat field's shadows stay.

Clouds → Variety makes the clouds differ in how dense they are,
the same shapes white and puffy or heavy and dark (up to four times either
way; fair weather puffs lighter). Lifetime (`cloud_lifetime_minutes`, 15
unless set, 0 for still clouds) makes each one live: it forms from its
densest cores, grows whole, then dissolves from its thin edges inwards and
leaves clear sky for a while before forming again, each at its own pace;
it is all in the shader, from the clock, with the shadows following.

The Sky button opens the clouds' map seen from above, centered on you (the
red dot; the line is the wind), to paint the sky with the mouse: Coverage
(how many clouds: clear blue to cloudy white), Heaviness (puffy white to
heavy gray) and Kind (fair weather puffs green, heaps yellow, towers red),
left button more, right button less, "Back to normal" or the Normal sky
button for the sky the settings make. The middle of each range is what the
settings say, so the sliders still work over a painted sky. The clouds are
placed again a few times a second while painting; the painted sky is saved
with the map (cloud_paint.h). The fourth, Flocks, is where the flocks are
(below); it starts empty everywhere.

The flocks are a thin layer of small white clouds high up, the ones in
patches and rows (altocumulus, cirrocumulus: "mackerel sky"), all
procedural: the Worley cells of the shape noise, a little longer along the
wind, pushed off their grid by a broader noise, bunched in rows across the
wind, slowly boiling, flat at the bottom and rounded on top, thin and white.
Each has a texture of its own, sized to it: the fine noise frays its edge
into tatters, broad lumps make its middle thick in knots and thin, almost
open, between them (fading out far away, where it would sparkle).
Marched into the same dome after the big clouds, seen through them, with a
light dappling of shadow on the land. Clouds → Flocks:

| Setting | Key | What it does |
|---|---|---|
| Size | `flock_size_meters` | how big each one is: grains (100 m) to big heaps (1500 m); the layer is about a third of that thick |
| Amount | `flock_amount` | how many: a few strays to packed in rows |
| Trend | `flock_trend` | all of them at once: dissolving (-1), steady (0), growing (1); at 1 from none to all in five minutes of the sky's clock (the console's `time` speeds it up); a new Amount starts over from it |
| Height | `flock_height_meters` | the bottom of their layer (4500 m) |
| Density | `flock_density` | how opaque they are: a veil to solid little heaps (0.5) |
| Shade | `flock_shade` | how gray their middles get in their own shadow, 0 all white (0.3) |

Only where the sky is painted for them: none at all costs nothing.

`tools/cloud_bake` grows clouds instead of drawing them: a small
simulated atmosphere (air as a fluid on a grid, carrying heat, vapor and
cloud water) over unevenly warmed ground; the warm patches send up
thermals, the rising air cools, condenses into cloud at its dew point and
the latent heat lifts the cloud further, until a stable layer caps it. The
cloud is baked into the atlas's shape format, as
`data/clouds/<type>_<n>.cloud` (humilis, mediocris, congestus, storm,
stratus; n from 1 to 16): a type's simulated clouds take the place of its
shapes made of blobs. Many at once: `tools/cloud_bake --type congestus
--count 16` grows seeds 1 to 16 into `data/clouds/congestus_1.cloud` ...
`congestus_16.cloud` (`--seed N` to start elsewhere, `--out-dir` for
another directory). Each takes some minutes at `--grid 128` (the default),
on all cores. The atmosphere's knobs are on the command line
(`tools/cloud_bake --help` lists them; `--type congestus --cap-height 1800
--show` prints what a run would use and stops): ground temperature, how
fast the air cools with height below and above the cloud base, where the
stable layer that caps the clouds is, the humidity, how warm, wide and
long lived the thermals are, the wind shear. `--grid 64` is eight times
faster for trying; `--preview cloud.pgm` writes the cloud seen from the
side and from above; `--name` writes it as another type. A window shows
the cloud growing while it is simulated (`--no-view` for none): drag to
turn it, the wheel zooms, Space pauses, Enter keeps the cloud as it is
now, N drops it and goes on to the next seed, Esc stops without keeping
anything; when it is done the window stays up with the last cloud until Esc. `--save-water file` keeps a simulation and `--water file` opens
it again in the window, to look at and bake without simulating.

The simulation runs on the GPU (a compute shader,
`data/shaders/cloud_sim.comp`; `make shaders` first), many times faster
than on the CPU; `--cpu` for the CPU, which it falls back to without a GPU.
The two do not agree to the last cell, but grow the same clouds. Every
couple of hundred steps it prints the GPU's milliseconds a step, and what
each phase takes (advection, physics, vortices, pressure; timed one at a
time, so they add up to a little more).

To find good settings, the fuzzer: `tools/cloud_bake --fuzz fuzz.ini`
grows clouds with settings picked at random from the ranges in the ini
(`tools/fuzz_example.ini` to start from), and
keeps every one, good or bad, for you to judge: `fuzz/1.png`, `fuzz/2.png`
... (`--out-dir` for elsewhere; it goes on from the first free number), each
the cloud from below, from the side and from above, and next to each
`1.txt` ... with what grew it. The window shows each one growing; N cuts a
run short (it is still kept), Esc stops. The ini:

    [run]
    type = congestus     ; the type the ranges start from
    grid = 64
    runs = 200

    [ranges]
    ; any knob, without its dashes, or vorticity or seed:
    ; low, high = uniform between them; one number = fixed
    thermal-warmth = 0.5, 3
    cap-height = 1200, 3000
    vorticity = 0, 0.08
    seconds = 1500

A good one is grown again, finer, from its txt: `tools/cloud_bake --params
fuzz/17.txt --grid 128 --out data/clouds/congestus_1.cloud` (the first line
of the txt says so). Anything on the command line changes what the txt
says; the txt changes what the ini says.

Clouds made elsewhere come in as OpenVDB files: `tools/cloud_bake --vdb
cloud.vdb` reads the file's density (the first float grid, or
`--vdb-grid name`) and shows it in the window, at its own resolution;
Enter writes it for the game (below), `--picture
cloud.png` draws it from below, the side and above. `--vdb-meters` is a
voxel's size (5 m unless given), `--vdb-density` scales how thick it looks,
`--vdb-up` says which of the file's axes is up (z unless given, as these
EmberGen exports have it; y, x, or -z for upside down), `--vdb-size 128`
makes it smaller first (its longest side 128 voxels), to see
what a game sized one keeps. A read cloud is drawn lit as it would be in the
game: a sun anywhere (the arrows move it), the light scattered many times
inside the cloud (Wrenninge's octaves), the silver lining with the sun behind
it, the sky's light from above; `--sun`, `--sky`, `--exposure` for how
bright.
The reader is ours (`tools/vdb_read.c`, plain C, no OpenVDB library): float
or half float grids, uncompressed (EmberGen's "compression: none"), zip, or
Blosc (Houdini's default; BloscLZ, LZ4 or zlib inside). Houdini's files have
y up: `--vdb-up y` for them.

For the game a read cloud is written with its real density, not as a
shape: `tools/cloud_bake --vdb cloud.vdb --out-dir data/clouds --no-view`
writes the next free `data/clouds/cumulus_<n>.cloud` (`--name` for another
name, `--out file` for a given one): the density, its longest side across
192 texels at most (a smaller cloud keeps its own), each side a multiple of
4, 0..255 with the 95th percentile of the cloud at 255, zlib packed (a few
hundred KB). A file holding a field of clouds (many small ones apart, like
Houdini's Cumulus_Cloud_1) is cut into its clouds with `--split`: each piece
that touches no other is a cloud of its own, the crumbs dropped.

The game loads `cumulus_1.cloud` ... `cumulus_64.cloud` into an atlas of
their own, each at its own size (side by side in rows), compressed to BC4
on the GPU (half a byte a texel; a byte where the GPU has no BC4 in 3D):
the 22 shipped (the ten EmberGen cumulus, Houdini's congestus, eleven small
ones cut out of its cumulus field) take about 45 MB. When there are any,
every cumulus stamp of the sky uses them instead of the shapes made of
blobs: one picked at random (for fair weather puffs the flatter of two, for
towers the taller), smaller or bigger with its kind, turned and mirrored at
random; only overcast sheets keep the old shapes. They are marched as they
are, lit the same way (the shade towards the sun from short steps first,
so their lumps shade each other); near, their thin parts frayed a little by
the fine noise.

Since nothing on the map reaches the clouds, they are marched not for every
pixel but for every direction around the eye, into a dome (an octahedral
map). Each frame
marches a sixteenth of it again and blends that with what it held (its
size is `cloud_dome_size`, Graphics → Clouds → Sharpness; 0, Auto, makes it
about 1.4 times the screen's height: 2048 at 1440 pixels; with the eye
under the clouds it holds the sky above the horizon only, twice the texels
for it): turning
the camera costs nothing, the cost does not grow with the screen's
resolution, and the jittered marches average into smooth clouds. The dome
is seen from the eye alone, so while the eye moves enough for the nearest
clouds to shift (a few meters a frame under them, any move inside them) all
of it is marched each frame, coarsely, a march for every 2x2 texels; when
the eye stops the fine detail comes back within a few frames. The still
dome's refresh goes by the clock, not by the frames: all of it every
`cloud_refresh_seconds` (Refresh, 0.1 s), a sixteenth at a time (none in a
frame when none is due, a few in a slow one), each new march mixed in so
that what a texel held fades over `cloud_memory_seconds` (Smoothing,
0.3 s): the same sky at 30 and at 400 frames a second, and cheaper the
faster the machine. Longer Smoothing is smoother but trails behind moving
clouds; longer Refresh is cheaper but they move in steps. The shape
noise is read through a slow wobble, so its repeats never line up into
rows of identical clouds. The screen then only looks
each pixel's direction up in the dome, unless the land is nearer. The
water still mirrors the flat sheet. Off, the flat clouds cost next to
nothing, for weak devices.


The northern lights (`aurora.c`, `data/shaders/aurora.glsl`): curtains of
light over the north (away from the noon sun), in a layer from 95 to 300 km
up. The sky's shader follows each ray up through the layer, over the round
earth (towards the horizon it meets it far away), in 24 steps of height; at
each the curtains are two bands running east to west, wavy, their waves
rolling, made of vertical rays that shimmer; bright at their sharp lower
edge, green, going red and violet higher up, a pink fringe at the bottom.
The water mirrors them; they add a faint green to the light from the sky.
The game says when (`aurora_set`; the World window's Aurora, the console's
`aurora on|off [strength]`); they show only in the dark (the sun 4 to 12
degrees under the horizon fades them in), and stay until put out.
`render.aurora_strength` (Sky & light) scales them.

Rain (`rain.c`, `data/shaders/rain*.vert|frag`): the simulation's rain
(`water.raining`: R, the Brushes' or the World window's Rain, the console's
`rain on|off [intensity]`) is also seen. The drops are thin streaks in a box
of 60 by 24 meters around the eye, slanted by the clouds' wind, stopping at
the ground or the water; nothing is simulated, where each drop is follows
from its number and the clock, and the box wraps around the eye. Where they
land, small rings spread on the ground and the water (splashes), and the
water's surface gets rings of its own (`water.frag`). While it rains the
sun and the sky's light dim, the haze thickens and the sky goes gray;
objects darken and shine as they get wet (the ground's wetness is the
simulation's moisture) and dry slowly afterwards. `render.rain_intensity`
(how hard: the number of drops and all the rest), `rain_streak_meters` and
`rain_splashes` are in Sky & light.

In the snow's months (`[render] snow_first_month` .. `snow_last_month`)
the rain falls as snow (`snow.vert|frag`): slow flakes, two or three centimeters
across, each falling at its own speed, swaying from side to side and
drifting with the wind more than a drop does, in a box of 44 m around the
eye; no splashes, no rings on the water, no wetting, no rainbow. Sleet (a
mix) for a week or so as the snow's months begin, rain again as they end.
The water the simulation's rain brings comes all the same (it is a game).

The rainbow (`rainbow.frag`): 42 degrees around the point opposite the sun,
red outside (each color its wavelength's, as bright as the eye sees it:
yellow and orange the brightest), a fainter secondary at 51 degrees with its colors reversed,
the sky brighter inside the first and darker between the two (Alexander's
band). It hangs 450 m off, in the rain: drawn over the sky and the land
farther than that (its depth is that of a point that far along the ray),
never over anything near. So it shows only with the sun above the horizon
and under 42 degrees, and never at night. The game says when
(`rainbow_set`; the World window's Rainbow, the console's
`rainbow on|off [strength]`); `render.rainbow_strength` scales it.
