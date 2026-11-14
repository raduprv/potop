# Props (potop_prop, prop_editor)

Rocks, dry stone walls, wells and wooden fences, made straight into
`.pobj` (`tools/prop_grow.c`): `tools/potop_prop --preset NAME
[--seed N] [--set KEY=VALUE]... [data/objects/name.pobj]`, or in a window,
`tools/prop_editor [prop_plans/name.prop]` (as the plant editor: the recipe
on the left, the prop on the right, made again on Apply or F5, or with
"At every change" a moment after each change; drawn with the game's prop
shaders; F12 a screenshot; View, Show: the
crevices, the edges or the sky the vertices carry, as gray).

Presets: boulder, river_stone, slab, crag, holed_rock, split_rock, pebbles
(rocks); dry_wall, ruined_wall, well (walls); rail_fence, picket_fence.
`potop_prop --list` prints them and every setting. A rock is carved from
a lump between an ellipsoid and a block (boxiness), made lumpy, split
flat (facets: their number and depth), its foot cut flat, grooved between
layers (with a tilt), holed through (or pitted), or several (a heap of
pebbles). A wall is such stones laid in courses, straight or round
(`ring_radius`: a well), narrower at its top (batter), stones gone from
its top (ruin). The surface comes off a grid (`resolution`: cells along a
rock, or along a wall's stone), and is simplified to `triangles` at most;
three levels of detail come with it, simplified again from the same
surface (a quarter, a twelfth, a fortieth of it; `OBJECT_MATERIAL_LOD_MESHES`:
meshes of their own in the same .pobj, only the full one solid), each
with how far off it is from the real surface (the header's `lod_errors`:
its shape, and its normals' lean over its triangles). The game draws a
level once that is less than `[objects] prop_lod_pixels` (0.5) on screen
(Settings, Objects: "Rocks' detail off by"; lower: the simpler ones only
farther away). Their moss where the full one has it (the editor's View, Detail level, shows
them). A recipe keeps how many variants were saved with it (`variants`):
`potop_prop --all prop_plans` makes every recipe there again, variants
and all (after the tool changes, the props as it makes them now).

Rocks and walls are textured triplanar (`OBJECT_MATERIAL_TRIPLANAR`): by
the model's own axes, one repeat every `texture_meters`, no texture
coordinates and no seams, holes and all. Their vertices carry what the
weather finds there (the tangent's x: how hidden, a crevice; y: how sharp
an edge; z: how much sky it sees), and `prop.frag` grows on them, as the
world has it: moss where it faces up and north and in the crevices,
never on the edges (greener when wet), lichen in colonies of round crusts
on the open dry stone, snow on top in the snow's months, wet stone
darker and shinier. `moss` and `lichen` (0..1) say how much (the object's
header carries them). Fences have texture coordinates: every piece of wood
along its grain from a random place of the texture.

Textures (three files under `data/`, named in the recipe as the game names
them: `color_texture`, `normal_texture`, `arm_texture`): seamless (they
tile), 1024 or 2048 square, the color without light or shadows, the normal
map OpenGL's way (green up), ARM: R occlusion, G roughness, B metal. None
named: the style's stand-ins (granite, sandstone, slate, limestone, wood,
old_wood), made once in `data/props/textures/` (`--textures STYLE` makes
them again). The wood's are laid out as `data/props/wood_template.png`
(`--wood-template FILE` writes it): the top three quarters the side grain,
running along (left to right; it tiles that way, a tenth of
`texture_meters` between the marks), the bottom quarter four ends' rings
(a piece's end uses the middle of its square, within the dashed line).

Barrels (kind barrel; presets wine_barrel, water_barrel, lying_barrel,
broken_barrel, tub, bucket): staves round a bulging profile (its radius at
its middle and at its ends), iron hoops round them, a bottom and a lid of
boards (`lid = 0`: open, a tub), lying on its side (`lying`), a bucket's
handle (`handle`); `wear` moves the staves and the hoops, and from 0.6
on breaks it (staves gone or short, no lid). Two materials: the wood
(textured as a fence's, every stave its own place of the texture, their
ends the end grain) and the iron (`metal_color_texture`,
`metal_normal_texture`, `metal_arm_texture`, or `metal_style`'s
stand-ins: rusty_iron, iron), not weathered. Its levels of detail are
made again with fewer rows and segments, the same staves.

In `objects.ini`, as any object: a boulder `physics = body`, a wall or a
well `physics = static` with `shape = mesh` (a hull would fill the well).
Triplanar models get no impostor.
