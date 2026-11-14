# Plants (potop_plant)

`tools/potop_plant` grows plants, procedurally, straight into `.pobj`
objects (`make tools` builds it): trees and bushes, and the small things
on the ground. `tools/plant_editor` does the same in a window, the plant in
view (see "The plant editor" below); both grow from the same code
(`tools/plant_grow.c`) and the same recipes.

```sh
tools/potop_plant --kind oak --seed 7 \
	--bark data/textures/bark_diff.jpg --bark-normal data/textures/bark_nor_gl.png --bark-arm data/textures/bark_arm.jpg \
	--leaves sources/oak_leaf.png,sources/oak_leaf2.png data/objects/oak_7.pobj
tools/potop_plant --kind fir --seed 2 --twigs sources/fir_sprig_m3.png data/objects/fir_2.pobj
tools/potop_plant --kind rosette --leaves sources/dandelion_leaf.png --heads sources/dandelion_head.png data/objects/dandelion_1.pobj
```

Kinds, one for each sort of plant (a first guess at each from how it
grows; the options below tune them):

| kind | what |
|---|---|
| `oak` (`deciduous`) | a round crown, branches spreading |
| `birch` | tall and slender, the finest branches hanging |
| `willow` | the finest branches hanging long |
| `weeping_willow` | thick limbs arching out, long leafy whips hanging from them almost to the ground |
| `pollard_willow` | cut back year after year: a short thick gnarled trunk, a knob for a head, a broom of straight shoots |
| `bush` | several stems, round |
| `spruce` (`conifer`) | a tall cone, the branches hang and turn up at the tips |
| `fir` | a cone, the branches level, the sprays flat |
| `fir_squares` | the fir of whole boughs: each bough out of the trunk one picture of a whole bough seen from above along it, and a second one a few centimeters under it (turned over), arched; its own atlas (`fir_branches`: 2048, squares of 1024); a test |
| `pine` | a long bare trunk, a high crown of few long branches |
| `thuja` | a narrow column of short branches pointing up |
| `juniper` | a few stems, a dense narrow shrub |
| `cherry` | tall, a leader up the middle, the branches in tiers reaching up |
| `plum` | lower, round and dense, thin tangled branches, two stems (also for corcodus) |
| `sour_cherry` | small and dense, its thin branches hanging |
| `tuft` | blades in a star |
| `rosette` | leaves lying in a ring, stalks with heads (dandelions, plantain) |
| `fern` | fronds arching out, each a long picture on a bent strip (`--frond-angle D`: how steep they start, 58; `--frond-droop X`: how far they arch over, 0.7; the inner ones shorter and steeper) |
| `flower` | stalks with a few leaves and a head |
| `mushroom` | a stem and a cap, made of the recipe's shape: one, or a few together (see below) |

**The winter's snow baked in** (`fir_squares`, off unless the recipe says `snow_bake = 1`,
in the editor "Snow baked in"): the tool lets snow fall straight down on the
tree, a path every 2.5 cm. Where one meets a bough's top card on its
picture (not through the gaps between its shoots) about half of it stays,
the more the flatter the card; the rest falls on to the next bough down.
What each bough caught, smoothed and in lumps, is the snow lying on it: a
mesh of its own over the card (the material `snow`), up to some 13 cm
deep, thin and grey blue at its edges where the needles show through. Each
bough sags under its load and under what rests on it from the one above,
the more the farther out (a beam held at one end); where its end would go
into the snow of the one below, it rests on it and that one carries part
of its load (a few rounds, top down). It is a model tuned to look right,
not a simulation: `snow_sag` (the fir's 0.06) scales the sag; it is
written into the object (`.pobj` version 6, `OBJECT_FILE_SNOW`) as where
each vertex goes under all of the snow. The game moves every vertex that
far times the snow on things, so the snow grows on the boughs as it
settles and they bend as it does, and both go as it melts (`plant.vert`);
without snow the snow mesh is not drawn. `POTOP_SNOW_DEBUG=1` prints each
bough's load and sag.

Mushrooms are made here, of the recipe's shape (all of it of `--height`,
the biggest one's): a stem, a tube along an arc, and a cap on it, a solid
of revolution, the gills under it (a few hundred triangles each; solid,
lit by their own normals; the wind leaves them be: `OBJECT_FILE_STILL`).
Each with `--mushroom KEY=VALUE`, or in the recipe as `KEY = VALUE`:

`preset = NAME` starts from what a common one looks like (all its values;
the rest of the recipe after it changes them): `fly_agaric` (the
default), `porcini`, `chanterelle`, `parasol`, `honey_fungus`,
`field_mushroom`, `death_cap`, `russula`, `saffron_milk_cap`,
`shaggy_ink_cap`, `puffball` (the editor's "Start from").

| key | what |
|---|---|
| `size` | all of it, the cluster's spread too, this many times as big (1; a preset keeps it) |
| `stem_length` | the stem's length (0: what the cap leaves of the height) |
| `stem_thickness` | its radius at its middle (0.07) |
| `stem_taper` | its radius at its top, of its foot's (0.8; above 1: wider up, a chanterelle's) |
| `stem_bulb` | its foot swollen by this much of its radius (0.6) |
| `stem_volva` | a cup round its foot (0..1: a death cap's) |
| `stem_bend` | it bends towards the cap (0.15; 1: about 45 degrees), each its own way |
| `ring_height`, `ring_width`, `ring_droop` | a skirt this far up the stem (0: none), out from it by this much of its radius, hanging |
| `cap_width`, `cap_height` | the cap's radius and height (0.42, 0.26) |
| `cap_dome` | 0: a cone .. 1: a dome |
| `cap_funnel` | its middle down and its rim up (0..1) |
| `cap_umbo` | a bump in its middle (0..1) |
| `cap_curl` | its rim bent down (+) or up (-), by as much as the cap is high or, a flat one, wide |
| `cap_wave` | its rim wavy and uneven (0..1) |
| `underside` | `gills`, `pores` (a sponge: boletes) or `ridges` (forked, running down the stem: chanterelles) |

The colors (sRGB `r, g, b`, 0..1) and their patterns, for the texture made
from them: `cap_color`, `cap_rim_color` (towards the rim), `cap_center_color`
and `cap_center` (how far out the middle's color goes), `zones` (rings of the
middle's color: a saffron milk cap's), `streaks` (fine streaks out from the
middle: a death cap's), `spots` and `spot_size` (warts), `scales` (patches in
rings: a parasol's), `spot_color` (the warts' and scales'), `gills_color`,
`stem_color`, `stem_pattern_color`, `stem_net` (a net up the stem: a
porcini's) and `stem_bands` (zigzag bands down it: a parasol's), all 0..1.

Their texture is laid out as `data/mushrooms/mushroom_template.png`
(`--mushroom-template FILE` writes it): the cap from above (the big
circle), the gills from below (the small one; the stem's stub in its
middle, the cap's rim round it), the skirt (the band right), the stem round
(the wide band: its left edge meets its right), each on its own. A texture
painted on it is the recipe's first `--leaves` picture (in the atlas, a
square cell: `| small` for a small one). Without one, one is made from the
colors above, as `data/mushrooms/<the object's name>_colors.png`. `--fruit-cluster N,M` grows
N to M of them together, the first the biggest in the middle, the others
smaller round it, as far apart as they are wide, leaning out.

Trees: a trunk, branches out of it spiralling by the golden angle, smaller
ones out of those, each a bent, tapering tube, as long as the crown's
outline (round, cone, column or dome) is wide there (Weber and Penn's
method, simplified). The same `--seed` grows the same plant, another one another of the kind;
`--height` sets its size (else the kind's, give or take 15%). Leaves,
twigs (sprigs that stand for the finest branches of a tree, two crossed
cards each) and heads are pictures on alpha, the stem at the bottom middle;
a list picks one at random for each card. The cards are lit as the crown
(or the sky) is, not by their own facing, so they do not flicker as the
plant turns; each faces the way it is lit. The bark (and the stalks of
small plants) tiles round and along (`--bark-meters`, 1.5 m a tile).

How full a tree is: `--leaves-per-branch N` (leaves on each of the finest
branches; the kind's own, `oak` 10) and `--branches N,N,N` (branches out
of the trunk, out of each of those, and so on; `oak` 9,7,6, `spruce`
50,14, `fir` 45,12, `pine` 16,9; fewer numbers leave the rest as they are).
An oak is 9 x 7 x 6 = 378 finest branches; each leaf is a card of
two triangles. `--twigs` fills a crown with fewer triangles: a sprig picture
(two crossed cards) for each finest branch, `--twig-scale` times as long as
it (above 1 they overlap and close the crown); conifers and bushes have
more along their other branches and one at each tip, and fir sprays lie
flat. A twig lies along the way the branch it stands for grows (bent
down, hanging), from its foot to its end; a finest branch longer than
`--twig-length M` meters (the weeping willow's whips: 0.6) has twigs one
after the other along it instead, each the way the branch goes there,
and no bark (the twigs' own stems are the whip).
`--branch-length X` makes the trunk's branches X times as long.
The crown's shape, each the kind's unless given: `--crown-start F` (where
the trunk's first branches are, of its height), `--leaves-from F` (foliage
only above that much of the height; lower down, in the shade, the branches
are bare but for their tips, fewer the lower: spruces and firs have it),
`--droop X` (the branches bend down under their weight, like a loaded beam:
more the longer they are and the farther out), `--tip-lift X` (their tips
turn up again, over their outer third: the spruce's), `--collar X` (a
branch's foot swells where it joins its parent, by that much of its radius,
and sits deeper in it, rather than going in like a toothpick: the
broadleaved have it, the conifers hardly) and `--root-flare X` (the trunk's
foot swells towards the roots). The thinnest branches have three or four
sides, short ones fewer segments.
How thick they are, each the kind's unless given: `--trunk-thickness F`
(the trunk's radius at its foot, of the height: 0.03 for an oak's,
0.022 for a thuja's), `--trunk-taper F` (its radius at its top, of its
foot's: the conifers' 0.08 thins to a hair at the top, 0.25 or so keeps
the top branches on something), `--branch-thickness F` (a branch's
radius at its foot, of its parent's where it leaves it), `--branch-taper
F` (a branch's radius at its tip, of its foot's) and `--thinnest-branch M`
(no branch's radius less than M meters: 0.002).
The trunk's shape, each the kind's unless given: `--stems N` (trunks),
`--fork-height F` (they grow as one trunk up to that much of the height,
then part, as two trees grown together; 0: apart from the ground, as a
bush's), `--extra-stem-chance P` (by the seed, one stem more than that: a
forest of one recipe has a forked one here and there), `--gnarl X` (the
trunk and the branches wander, swell and narrow: an old oak; more segments
the more gnarled, none more at 0), `--kinks X` (a branch turns away from
each one that leaves it, the zigzag of oaks and fruit trees). Bent by the
wind, as the trees on the rocks by the sea: `--lean-degrees D` (the trunk
leaves the ground that far from upright), `--lean-length F` (and goes so
for that much of its length), `--straighten F` (then turns up again: 1
upright by its top, 0 not at all) and `--wind-side F` (the branches on the
side the wind comes from, against the lean, shorter and fewer: a flag).
The crown of a leaning tree is not over its root; standing, only the
trunk is solid in the game, so it collides where the wood is.
Through the seasons (see "Objects": the plants' shaders): `--moss X` (the
bark's north side; the broadleaved and the big conifers have some, the
bushes none), `--tips X` (the foliage's lighter tips), `--autumn X` (how
much the foliage turns; 0 for the conifers), `--autumn-colors
R,G,B,R,G,B` (sRGB 0..1: birch yellow, cherry red, oak rusty...),
`--falls 0|1`, `--autumn-months F,L` (it turns from the start of month F,
falls by the end of L), `--leaves-month M` (bare until then) and
`--snow-sag X` (how far the branches sag under the winter's snow, of the
height, at the crown's edge). A picture's months: pictures beside it named
`name_m1.png` .. `name_m12.png` (see "Objects"), all of one size (they are
cut to one box, so they lie on one another exactly). A plant made before
(`.pobj` version 2 or 3) is drawn as any object, without the seasons:
make it again.
Levels of detail, for the distance (the game does not use them yet): a
tree's branches are written in the order they go away, the trunk's
branches first, so that the first part of them is the tree without its
finest branches (those that hold the twigs or the leaves, and the fruit's
stalks), a smaller part without the next ones too, and none (only the trunk
and the foliage); the counts are in the material (object_format.h,
OBJECT_MATERIAL_LODS). The foliage's cards are in a random order
(OBJECT_MATERIAL_SHUFFLED): any first part of them is the crown thinned
out evenly. The editor's View shows them: "Detail level" and "Foliage
kept".
With both `--twigs` and `--leaves`, `--twig-share S` (0.7) of the finest
branches are twigs and the rest keep their leaves; `--loose-leaves N` puts N
leaves per meter along the other branches too (they break the outline the
twigs leave and fill the crown's inside).

Fruit (`--fruit FILE[,FILE...]`, with `-normal` and `-arm` like the
others): cherries, nuts, cones, pods, on the finest branches and the twigs.
`--fruit-shape` sphere (oval with a width), spindle (cones), pod (flattened
and bent a little) are small solids of revolution, a few dozen triangles
each, their picture wrapped round them: each ring takes the picture's row at
its height, as wide as the solid is there, so a picture that shows the fruit
from the side, filling it, gives the solid its look (the back mirrors the
front); card is two crossed cards. The picture's top is the end it hangs
from for hanging fruit, its bottom otherwise. `--fruit-size L[,W]` (meters;
the width from the picture's shape unless given), `--fruit-hang` down (on
its stalk), up (standing on the branch: firs), out (out of it: pines),
`--fruit-stalk M` (a thin bark tube), `--fruit-cluster N[,M]` (that many
from one point, each leaning its own way), `--fruit-per-branch X` (clusters
per finest branch or twig, on average) and `--fruit-zone` top (the upper
crown: cones), outer (the outside, where the sun is), all. Each kind has its
own hang and zone (spruce down at the top, fir up at the top, pine out, the
broadleaved down on the outside). Fruit is its own material (`fruit`), from
the atlas, lit by its normals, passable.

The cards' normals light them as the crown is lit, the same from either
side (`OBJECT_MATERIAL_LIGHTING_NORMALS`; a real leaf's back has its normal
turned round), so a crown seen from under it is not dark where the cards
show their backs.

Foliage deep in a crown is darker (in the shade of the rest: by how far in
from the crown's outline it is at its height, and a little low down), and
each card a little lighter or darker than the next, so a crown seen from
afar shows clumps of light and shade rather than one flat green. The
shade rides in the size of the tangent's w (its sign is the bitangent's);
`object.frag` reads it on cut out faces only, and every other model has 1
there.

Only the trunk is solid (a bush's stems are its trunk): it is the material
`trunk`, the branches are `bark` (the same textures, passable) and the
leaves `foliage`. Physics, the paths and the water leave out what is not
solid; the mouse still picks a tree by its crown. Small plants have nothing
solid: one walks through them (the water can still tear them out).

The pictures live in atlases shared by every plant of the game:
`data/textures/conifers` (4096 px, all 128 cells tall ones, 256 x 512:
the sprays are long), `broadleaved` (trees, bushes; 4096 px, 512 px square
cells, a row of 256 x 512 tall ones) and `small_plants` (the rest; 256 px
square cells, four rows of 128 px small ones, two rows of 128 x 256 tall
ones; an `.ini` made before there were small cells has none, until it is
taken out and the plants made again), each a `.dds` (BC7, with
its levels: `engine/plant_atlas_file.h`; read as it is where the GPU has BC7,
decoded where it does not) with an `.ini` that lists which picture (and
its months picked by hand) is in which cell. A picture's months take cells
of their own, of its shape (a conifer of 3 sprays with 3 looks through the
year: 9 cells). No maps: the plants' shaders
take none for the foliage. A picture not in its atlas yet goes in the
first free cell that fits (one half again as tall as wide in a tall cell;
the small plants' little ones, flowers' heads and leaves of 10 cm or
less, in a small one; `| small` or `| big` after a picture in a
recipe, or its button in the plant editor, chooses);
one already there keeps its cell. Each run makes the atlases again from
the `.ini`'s list (a line taken out frees its cell; a picture drawn again
shows) and writes them only if they changed. `--atlas` uses another. The
objects do not carry the pictures: they name the atlases and the bark
(under `data/`: a bark from elsewhere is copied to `data/textures/`, unless
another picture of that name is there), and the game loads each file once for every
plant that names it (a `.pobj` of version 2: see `engine/object_format.h`;
those of version 1, with their textures in them, are read as before).

The game picks up models and textures written while it runs: once a
second it looks at the time of each object's file (the `.pobj` or `.e3d`,
and an `.e3d`'s texture files), and loads again what was written since
(and not in the last half second, so a file still being written is left
alone). Every object of that model shows the new one at once. So
`potop_plant` or `potop_convert` can be run again and again beside the open
game, the tree in view. The collision shape stays the old one until the
game starts again (physics and the paths were built from it).

Small plants (tuft, rosette, flower) can be given a shape of their own
(`herb_*` in a recipe; the editor's Shape section; `--herb KEY=VALUE`,
`--herb-preset NAME`): the leaves round its foot (how many, how much their
sizes differ, how steep they rise and give or take, how far they arch
over, the inner ones smaller and steeper, a stalk under each and how steep
it rises), the flower stalks out of its middle (how many at least and at
most, their height of the plant's, their lean, their radius, the leaves up
each), the heads (on top, or several up the stalk's top part: a spike, and
their size). Presets: burdock (big leaves on long stalks, rising and
drooping, no flower stalk), plantain, dandelion, daisy, foxglove (a spike
of bells), sunflower (leaves on stalks all the way up its stalk, round it
by the golden angle, smaller up it: `herb_stalk_leaves_from`, `_to`,
`herb_stalk_leaf_size`, `_taper`, `_rise`, `_arch`, `herb_stalk_petiole`,
`herb_stalk_leaf_spiral`; its head facing the side,
`herb_head_tilt_minimum`), lingonberry (a low mat: `herb_runners` stems winding along the ground,
`herb_runner_length`, `_forks`, `_wander`, `_thickness`, leafy sprigs up
along them, `herb_runner_leaves`, `_leaf_size`, `_leaf_rise`, `_leaf_turn`;
its leaf pictures sprigs: creeping juniper, ivy, thyme the same way), mugwort (a clump of reddish stalks, cut leaves
up their lower part, plumes up their top part leaning out of them:
`herb_head_lean` degrees, the heads under the top one), grass_clump. A recipe without them grows as
the kind always did.
The leaves' stalks (petioles) are tubes thick where they leave the plant
and thinner to the leaf: `herb_petiole_thickness` and
`herb_stalk_petiole_thickness` their radius at their foot (meters at the
leaf size; 0: by the leaf's size), `herb_stalk_petiole_rise` how steep
the stalk leaves' rise (0: 37 degrees). `herb_stalk_leaf_droop`: the
lowest leaves up a stalk hang this many degrees lower than the topmost
(their stalks half as much), the ones between in between;
`herb_stalk_leaf_scatter`: round the stalk, give or take this many degrees
from the golden angle's spiral (no plastic Christmas tree).
All the leaves (round the foot and up the stalks): `herb_leaf_fold` folds
them along their middle, a V (0..1: they show from the side too),
`herb_leaf_twist` rolls each round its middle at random, give or take so
many degrees, `herb_leaf_light` (0..1) turns their faces to the light, the
object's +Z up 45 degrees, each its own way give or take
`herb_light_spread` degrees round; `herb_head_light = 1` tilts the disc
towards it too, and `herb_head_neck` (0..0.5) bends that much of the
stalk's top over into the disc's tilt. Turned to the light when grown,
not in the game: in `data/objects.ini` give such a type `yaw_spread = 30`
(turned at random only that many degrees either way of its +Z; 0, the
default, all the way round as before) so they all face about the same way.
Without a bark picture the stalks and branches are a plain color:
`stalk_color = R, G, B` (sRGB 0..1; the editor's Pictures section, by the
bark; `--stalk-color R,G,B`; none: the kind's, a tree's brown, a small
plant's green). With a bark picture it is the picture's own.
The head on top of a stalk (`herb_head_shape`): 0 two cards crossed
(flowers with a body: hyacinths), 1 a disc on a cup (flat flowers:
dandelions, daisies: the flower from above on a disc cut out by its alpha,
its middle up a little, its rim down, tilted at random up to
`herb_head_tilt` degrees; under it a cap of a sphere, `herb_cup_size` of
the disc's radius, `herb_cup_depth` deep of its own (0.7: a third of a
sphere), with the flower from below: `name_under.png` beside the head's
picture, a square picture of it from below, centered; without one, the
leaves' green), 2 the disc alone. `herb_disc_curve` (-1..1) the disc's
top: 0 as it always was, below 0 hollow (its middle under its rim: a
sunflower's, -0.8), above 0 domed. The cups go away far off (the plant's
third level of detail); the discs stay.

After the tool changes, `tools/potop_plant --all plant_plans` (from the
project's directory; `--data DIR` before it) grows every recipe there
again, each into its own output (variants: Save them in the editor).
