# Objects

3D objects are Eternal Lands `.e3d` models, kept in `data/objects/` with
their textures next to them (the names inside the model must match, in any
case). DDS textures are decoded to plain RGBA at load time, because most
phone GPUs cannot read the DXT compression; textures are shared between the
models that use them. Cut out textures (leaves, reeds) get the colors of
their opaque pixels bled into the transparent ones before mipmapping, so
foliage does not grow dark fringes in the distance.

Every object is an instance of a type from `data/objects.ini`: a model plus
its size range, draw distance, shadows, footprint, how it takes part in the
physics (see Physics below) and where it grows. The keys are explained at the top of the
file; a `[defaults]` section sets values for the types after it.

New maps are populated when they are generated (and with V): for every type,
in the order of the file, a jittered grid of candidates is laid over the
map, each kept with a probability from the type's number per hectare and
from the spot: soil, slope, height, standing water, patches (all trees share
one forest noise field, grass grows outside it, flowers in their own small
patches) and valleys (tall grass, reeds and pebbles seek the low ground, big
rocks the ridges). Solid things keep a footprint free, so trees do not grow
through rocks and grass does not grow out of trunks. The result depends only
on the terrain's seed. A 1 km map gets about 250 000 objects (mostly grass)
in under half a second.

The plant brush (G) plants one category at a time inside the brush circle,
up to the density the types have inside their patches; it keeps footprints,
slopes and water rules, but leaves the soil and the place to the player.
Lily pads only grow on water: flood a pond, press V. They float on the
surface and follow it.

Objects stand on the lowest ground under their trunk and follow it when the
terrain changes. They are sorted into a grid of 32 m cells, and within a
cell by draw distance, so a frame only looks at what can be seen: grass
stops at 70 m, trees go on to the horizon. Over the last fifth of its draw
distance an object fades out with an ordered dither (no blending needed);
`[objects] draw_distance_scale` in settings.ini scales all draw distances.

Objects are drawn GPU driven: every object has a fixed slot on the GPU
(position, size, rotation, model), and only the slots of objects that
changed go up (moved by the physics, laid down by the water, put back on
the ground, added, removed). Each pass (camera, far and near shadow map) the
CPU only marks the grid cells in view; a compute shader
(`objects_cull.comp`) tests every object in them (draw distance, its box
against the frustum), writes the visible ones grouped by model into the
pass's instance buffer, and fills the indirect draw arguments: each
material of each model is a single indirect draw call however many objects
use it. On a 4 km map the slots take about 128 MB of video memory. The
camera's pass also leaves out what is hidden: before the water, the scene's
pass pauses and folds its depth (terrain, objects) into a pyramid (Hi-Z,
`hiz.c`: each level the farthest depth of the four texels under it), and
the next frame's culling, before the scene's pass, leaves out an object
whose box is behind that depth over its whole outline on screen (a
mountain or a house between it and the camera), tested with the camera
matrix the pyramid was made with. Being a frame old, something that moves
can hide what was behind it for a frame; what turns into view is outside
the old pyramid, so it is drawn. (On SDL older than 3.2.30, NVIDIA cards
drew objects from stale culling, "ghosts" with another's place and size,
once the window had changed size: see "Building". Two sets of instances
swapped each frame and a widened view worked around it; gone since.)
The pyramid's level 0 is made by a fragment shader
and the rest in a storage buffer that the culling reads, never by a
compute shader sampling a texture written in the same frame: SDL_GPU's
Vulkan backend does not order such reads after the write (its barrier
into the sampled state covers only the vertex and fragment shaders; the
sync validation layer flags it), and the culling, reading a half made
pyramid, hid or showed objects wrongly for a frame, more often at high
resolutions (full screen). A count and a write that decide twice could
also disagree (see `objects_cull.comp`): the write now takes the count's
decision. They cast
shadows (leaves and twigs through their alpha, so the shadows have holes):
big things into both shadow maps, small ones (grass, flowers) only into the
near one, lily pads none.

Far away, the models worth it (150 triangles or more, at least 1 m across
their bounding sphere's radius) are drawn as impostors: pictures of
themselves (`impostors.c`). Each such model gets an atlas of 8 x 8
pictures, 64 pixels each, taken from all round and from above (the upper
hemisphere unfolded on a square, hemi-octahedral), of its color and of its
normals and depth. An impostor is a square facing the eye, showing the
three pictures taken from nearest where the eye is, blended; each picture
is read where the eye's ray through the square meets that picture's own
plane, so the three line up. It is lit like the model (sun, shadow maps,
clouds' shadows, the sky and the ground, local lights, haze), its depth
pushed back or forth by the pictures' depth so it meets the ground where
the model would. The culling picks it once the object is smaller on screen
than `[objects] impostor_pixels` (its sphere's diameter, 64 pixels by
default: Pictures from, in the settings; the projection's focal length,
the scene's height and the size decide, so at a higher resolution they
switch later). Up to 64 pixels (a picture's size) it shows no less than
the model would; above that the picture is seen enlarged, a little soft; what tumbles (loose, laid down) stays a model. The shadow maps
keep drawing the models. `[objects] impostors = false` in settings.ini (Far
objects as pictures, in the settings) turns them off, `impostor = false`
on a type in objects.ini keeps that type whole, and the console's `objects
impostors all` draws every object that has one as its impostor, near too,
to look at them (`auto` puts them back far away); `objects impostors show`
(Show impostors, in the World window) tints them magenta.

The atlases are baked on the GPU when the game starts (or a model or one
of its textures changes on disk) and kept in `data/cache/impostors/`,
compressed, with a key made of the model's and its textures' files and
when they were written, and of how the pictures are made: only what
changed is baked again; the rest loads from the cache (about 2.8 MB of
video memory per model). `POTOP_IMPOSTOR_DUMP=<directory>` saves every
baked atlas there as pictures.

Plants move in the wind (Wind in the plants, in the settings:
`render.plant_wind_strength`, `plant_wind_direction_degrees`,
`plant_wind_gust_meters`; not the clouds' wind). All of it is in the
objects' vertex shader, nothing on the CPU: a plant leans with the wind,
more the higher up (with the square of the height, so its foot stays),
the lean swelling in gusts that roll downwind as waves, so a forest bends
tree after tree, and swinging a little on its own. It bends only as far as
the wind is over its stiffness, with the square of what is over it (the
push grows with the wind's speed squared): a bush stands still in a breeze
and moves in a storm's gusts, grass moves in any wind. Its leaves (cut out
materials) shake fast besides whatever the stiffness, each card as one,
from its foot (still, on its branch) to its tip. How much: a plant's pobj
carries its own (tools/plant_editor's View: sway, flutter, stiffness, by
its kind and height unless given; recipe keys `sway`, `flutter`,
`stiffness`): `sway` meters its top leans in a gale (at size 1),
`flutter` meters its leaves' tips shake, `stiffness` 0..1. A type's own
`sway`, `flutter`, `stiffness` in objects.ini (in its section, not
`[defaults]`) win over its model's; a model without its own (made before)
takes its type's, `[defaults]`' included, as before; each `[defaults]`
starts still. The physics, the shadows and the impostors keep them still.

Shadows come from two maps: a far one over the whole map, redrawn only when
the terrain or the objects change, and a near one over a square around the
point the camera looks at (`near_shadow_meters`, 100 m by default, at
`near_shadow_map_size` texels: about 2.5 cm each), fine enough for branches.
The near one is redrawn when the camera has moved a sixth of its width; its
shadows fade into the far map's towards its edges.

Levels of detail: the trees `potop_plant` makes now carry
them (see "Plants"), and the culling draws such a model coarser the
smaller it is on screen: its height under 300, 120 and 50 pixels
(`[objects] lod_distance_scale`, Branches as far as, moves
these: 2 keeps the branches twice as far) drops first its finest branches
(those holding the twigs or the leaves), then the next ones, then all but
the trunk; the foliage stays (thinned out at the last two levels to
`lod_foliage_kept` of it, 1 by default: all). It costs no more draws than
there are levels: each level of each model is one more indirect draw of
the start of the same index buffer. Nearing the next level, what it leaves
out thins out in a dither over the last 15% of the way, so the branches
do not pop away (the bark's vertices carry their branch's level, in the
tangent's size). The shadow maps draw the trees at least a level coarser
(the near one) or two (the far one): the finest branches' shadows are lost
in the leaves'. `[objects] lod = false` (Fewer branches far away) draws
every tree whole. Older trees and every other model have no levels: drawn
whole, as before.

Grass (`grass.c`, `grass.comp`, `grass.vert`, `grass.frag`): blades made
on the GPU every frame around the eye, nothing stored, on the ground the
terrain shows as grass (worked out as `terrain.frag` does: soil, slope,
sun, silt, bare rock, cliffs, snow). A compute shader looks at the spots of
a grid fixed to the world (so the blades stay put when the eye moves),
fine near the eye and coarser ring after ring farther out (each twice as
far, looking at every second spot of the ring inside it), the blades
thinning with the square of the distance and growing wider (each blade has
a threshold of its own and shrinks into the ground as the share kept there
nears it: they go and come back one by one, never a ring at once; the
blades of the coarser grids have the lower thresholds, so a ring's edge
changes nothing), so about as many show on the screen near or far
and the work grows only with the logarithm of the distance; they sink
into the ground over the last fifth of the distance. It keeps those in
view and writes them, with the indirect draw's arguments; the vertex
shader makes each blade from nothing, a strip of seven vertices up a bowed
curve: leaning a little its own way, with the wind (the plants' wind and
gusts, the trees' own, plus a sway of its own) and laid down the way the
water flows over it (the more, the faster and deeper; drowned deeper than
itself by 40 cm, it is gone), staying down and muddy in the silt the water
left. Its color is the ground's grass texture under it, with the terrain's
variation, darker at its foot and wet, a little yellow at the tip, some
blades dry. It is lit as the terrain is, the sun shining through it from
behind; it takes the shadows and casts none. The settings (Grass, in the
objects' tab; `[objects]` in settings.ini): `grass` (on), `grass_distance_meters`
(50: how far from the eye), `grass_density` (550 blades a square meter near
the eye; below some 300 it looks thin), `grass_near_density` (2: nearest the eye
that many times as dense, back to `grass_density` by `grass_near_meters`, 1),
`grass_falloff` (2.5: past the full disc, 6 to 20 m off, the share kept falls
with the distance to this power; 2 keeps the ground covered alike as the blades widen,
more is sparser far away), `grass_height_meters` (0.3), `grass_width_meters` (0.018),
`grass_tint` (multiplies the ground's color) and `grass_dry_share` (0.12).
The grass objects (the `tuft`s of objects.ini) are still planted as they
were: drop their `spawn_per_hectare` to leave the meadows to the blades.

The plants (`potop_plant`'s, `.pobj` version 3 with `OBJECT_FILE_PLANT`)
have shaders of their own (`plant.vert`, `plant.frag`, `plant_shadow.vert`,
`plant.glsl`), without the physically based shading (a plant has no use
for it, and its foliage is most of what is drawn), and no impostors (their
levels of detail do better far away). Each vertex knows what it is on the
plant (along its branch or card, how high on the plant, a number of its
card's or branch's own, how far out in the crown or how thick the branch)
and each material how it goes through the seasons. The bark takes its
normal map, if it has one, and grows moss on the side facing the north
(away from the noon sun), the more the thicker the branch and the lower,
in patches (`moss` in the recipe; none on bushes and the small plants).
The foliage is lit from either side, the sun shining through it from
behind, darker deep in the crown; each card a little lighter or darker
than the next, its tip lighter (the new growth, `tips`: the conifers'
most: a light yellow green at the sprays' ends). The year goes round
by months (`[render] month`, Month in the World window: 1 the start of
January .. 12.99 the end of December; `set render.month 10.5` in the
console). What the foliage looks like each month is in its pictures: a
picture's months are pictures of their own beside it (`twig_m3.png`,
`twig_m12.png`, `twig_m1.png`: green from March, a little snow from
December, more from January), each month showing its own; one without
shows the picture itself if that is a plain one (`twig.png`, the rest of
the year), else (`twig_m3.png`) the latest one before it (round the year); each of those pictures has a cell of its own
in the plant's atlas, the plant knows which of its cells shows which in
each month (the `.pobj`'s month cells), and the switch is at once, the
whole tree.
The autumn is the shaders': through a plant's autumn months
(`autumn_months` in the recipe, the kind's: October and November) it
comes to each tree a little earlier or later (its place decides) and
within it to each card in its own order, the outside of the crown and the
top first: a card turns to its species' autumn colors (`autumn_colors`,
two, a card somewhere between; `autumn`: how much, 0 for the conifers)
keeping the picture's light and dark, a little muted (ochre and rust
rather than flame red), and where the foliage falls (`falls`) the cards go
one by one past the middle of it (their shadows too), until the tree is
bare; it stays bare until its leaves' month (`leaves_month`, the kind's:
April). The winter's snow (`[render] snow_first_month`, `snow_last_month`:
December to February, Snow from and to in the World window) lies on the
upper sides of the bark, in patches, coming in its first month and going
in the last half month, and the branches sag under it (`snow_sag`: of
the height, at the crown's edge, the more the farther out; the trunk
stays). The foliage's lighter tips, its new growth, only from April to
September (`[render] tips_first_month`, `tips_last_month`; New growth from
and to in the World window): growing through April, going in the last
half of September. The plant editor shows it all (Seasons, and View's Month).
