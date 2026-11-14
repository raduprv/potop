# Ground textures from nothing

`tools/potop_texture [options] KIND NAME` makes a terrain texture, tiling
by construction (every noise in it repeats over the picture; the noise's
octaves on lattices turned against each other, the small things sampled
several times a pixel: no grid shows), at any size: `data/textures/NAME.png`,
`NAME_normal.png` (green up, as materials.ini wants by default),
and `NAME_roughness.png` (each grain, crystal, pebble, blade, puddle and
moss cushion its own), the normals as steep as the ground really is;
`NAME_height.png` only with `--height` (the game wants it only for a
relief, which always has it). `tools/make_textures.sh [SIZE] [DATA]` makes
them all again (sand, wet sand, clay, mud, dirt, wet dirt, grass, stone,
the rock's beds and relief), each wet one with its dry one's seed; the
names and meters in it should match materials.ini. Kinds:

* `sand` (2 m): two sets of wind ripples crossing (a long gentle windward
  side, a steep lee; their crests winding, forking, breaking off), grains
  of several minerals (quartz, feldspar, pale and stained ones, shell,
  rusty, a few dark heavy ones gathered in the troughs) sampled 3 by 3 a
  pixel, coarser grains and bits of shell laid on one by one, paler sand
  on the crests. `--ripples M` (their spacing; 0.11; 0: flat),
  `--ripple-height M` (0.008), `--grain MM` (1).
* `clay` (3 m): dry clay cracked into plates (big cracks, finer ones
  inside the plates ending where they will, hairlines), V shaped, the
  plates' edges curling up, the crust's grains and pits, grains and small
  pebbles on it. `--plates M` (the big plates; 0.35).
* `rock` (4 m): a mountain's layered stone (limestone): beds of stone one
  on another, thick and thin, swelling and pinching out, standing out or
  back as hard or soft; each cut into blocks by leaning joints; the
  blocks' edges worn round, their faces broken into planes, rough, a few
  fine cracks; gray-blue, each bed and block its own tone; moss cushions
  in the crevices and on the ledges (`--moss A`, 0..1; 0.45). The beds run
  along u: on a cliff (the terrain draws its sides with v up) they lie
  level.
* `canyon` (4 m): a canyon's sandstone: thinner beds of reds, oranges and
  creams, few joints, laminae in them, dark desert varnish streaking down;
  no moss (`--moss` gives some).
* `granite` (4 m): crystalline stone: few joints, its blocks worn into
  domes, sheets peeling off, its crystals (pale feldspar, gray quartz,
  black mica) each a little pit or bump, stained down from the joints;
  lichen colonies, gray-green and now and then orange (`--moss A`; 0.25).
* `dirt` (2 m): bare earth: crumbly clods, crumbs, damper patches, small
  dirty stones, bits of straw and rotting stems.

* `mud` (3 m): clay soaked, for materials.ini's `wet_texture`: smooth, its
  cracks closed into faint seams exactly where the clay of the same seed,
  size and `--plates` has them (they open in place as it dries), ripple
  marks the water left in places, drops' pits, darker shallow puddles.
* `grass` (2 m): grass seen from above, every blade drawn whole (tapering,
  bending, its tip higher: what stands higher is drawn over the rest, the
  deeper the darker), most in tufts, leaning together in patches, old dead
  ones under the living, each its own green, yellower patches, the soil
  and the thatch between; its normals gentler than the blades stand.
  `--color` its green.
* `wet_sand`, `wet_dirt` (2 m): `sand` and `dirt` after the rain, for
  materials.ini's `wet_texture`: the same picture (the same seed and
  options: it turns wet in place), the grains and crumbs settled and
  smudged, the colors deeper and darker, smooth, and water standing in the
  lowest places, between the ripples or the clods (`--puddles A`: the share
  of it under water; 0.18 rippled sand, 0.06 flat, 0.12 dirt).
* `stone` (4 m): granite's massive stone without its lichen, a rock's
  texture up close.
* `relief` (96 m): a rock face's big shapes, gullies down it where the
  water runs, buttresses between, ribs: only its normals and height, for
  materials.ini's `relief` (1024 pixels are plenty: `--size 1024`).
* `needles` (2 m): a conifer's litter, the material map's cover: needles
  layer on layer, the old ones gray brown under the newer rust ones, a few
  still green, in drifts lying mostly one way, a bare twig and a cone here
  and there, dark humus between. Longer and wider than real needles and
  fewer: at a millimeter or two a pixel real ones blur into dirt.
* `leaves` (2 m): a broadleaved tree's: leaves (pointed, curled at their
  edges, a darker midrib), the rotten ones dark under the newly fallen
  brown, ochre, a few yellow or red.
* `trampled` (2 m): earth pressed flat round houses: the dirt's, its clods
  cut off and smoother, dead grass pressed into it the ways people walk.
* `petals` (2 m): a blooming tree's petals on the ground, an overlay: the
  petals, and `NAME_mask.png`, each petal's turn (0 first .. 1 never: the
  ground between), so more show as more fall.
* `snow` (4 m): soft drifts, wind crust, bluish in the hollows, and its
  `NAME_mask.png`: where it settles first (patches, then all of it).

A mountain's rock (materials.ini `[rock]`): up close a massive stone
(`textures = stone_01`, `tiling_meters = 4`), from afar and through it its
beds (`macro_texture = rock_01`, `macro_tiling_meters = 24`: the beds'
tone and relief show through the stone up close too), on its steep faces a
relief (`relief = rock_relief`, `relief_meters = 96`). The terrain shader
projects a rock from the sides down to gentler slopes (from above its beds
would lie on the slope like paving), its macro scale too; moves its beds up
and down across the map (a broad dip, a wave, faults some tens of meters
apart) so they do not ring a mountain as contour lines; and bends its
normals by the relief (gullies and buttresses, darker in the gullies) and
by ledges along the beds here and there. And weathers it as seen from
afar: green on its flatter bits (more on the northern, -z side), dark
streaks down it (more down the gullies), rust and pale lichen over tens of
meters, its tone changing from face to face; snow lies lower and on steeper
ground down the gullies. Without `macro_texture` and
`relief` a rock is drawn as before but for the projections and the beds'
shift.

All: `--size N` (2048), `--meters M` (how much ground it covers:
materials.ini's `tiling_meters` should say the same), `--color R,G,B`
(sRGB; its average), `--seed N`, `--baked-light A` (how much light is in
the color, as a photograph has it: 0..1, 0.4; the game lights the normals
as well, so not all of it).
