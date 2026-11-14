# Houses (potop_house, house_view)

Houses, made straight into `.pobj` (`tools/house_grow.c`, the stand-in
textures in `tools/house_textures.c`): `tools/potop_house --preset NAME
[--seed N] [--set KEY=VALUE]... [--variants N] [--sections FILE]
[data/objects/name.pobj]`, or `tools/potop_house --all house_plans` for
every recipe there (`house_plans/*.house`, one line per setting, as the
props' recipes). `--list` prints the presets and every setting with what
it does; `--sections -` prints an `objects.ini` section for the house and a
`layouts.ini` one with its anchors (the door, a lantern by it, the
chimney's top, the balcony) for what stands by it. `--textures` makes the
stand-ins again.

A recipe starts from a **kind** at a **size**: `kind = hovel | cottage |
craftsman | townhouse | inn | barn | stone_house` and `size = 1 | 2 | 3`
(small to big within the kind) set everything else (the sizes, the
walls, the roof, the openings, what is added on), and the lines after
them override what they like. The kinds: a hovel is one low room under
a big thatched half hip, its eaves at head height, no base but
padstones, no glass (plank shutters), no chimney, nothing straight; a
cottage a peasant's, one floor and a loft under thatch, small windows,
an annex for the animals, a chimney from the middle size up, glass only
at the big size; a craftsman's house has a workshop below with a wide
door and few windows, the family above with glass; a townhouse a
merchant's, two to four jettied floors, glass, dormers, a gable to the
street; an inn is wide, with a covered walk along the front on posts, a
balcony, more dormers; a barn a big roof (thatch when small, tile when
big) over boarded walls, a two leaf door with braces (one leaf ajar now
and then), a hatch into the loft high in the front gable with the hoist
beam over it (`loft_door`), ventilation slits in the long walls
(`slits`), no windows; a stone house is stone to the eaves with
quoins, lintels and an annex. **Walls** (`wall`) are `timber` (framed,
plastered panels), `stone` (coursed), `wattle` (a few thin crooked posts
with daub between them, the wash fallen off in places to show the woven
rods), `log` (round logs laid flat, every other one reaching past the
corners, the openings cut through them and framed with planks, logs up
the gable too), `rubble` (rough field stones) or `plank` (vertical boards
nailed over a sparse frame, a dark gap between them, boards up the gable
too). Presets (`--list`):
`hovel`, `hovel_log`, `hovel_rubble`, `cottage`, `thatched_cottage`,
`craftsman`, `town_house`, `tall_house`, `gallery_house`, `hip_house`,
`inn`, `barn`, `stone_house`; `ground_floor_stone` puts a masonry
ground floor under a timber frame.

**Weathering baked in** (file version 7, `OBJECT_FILE_WEATHERING`; `--format
6` writes the older file for an older engine): every vertex carries how open
to the sky it is (64 rays against the house's own solid triangles and the
ground, within 4 m: the joints, the wall under the eaves and the foot of
the wall come out closed in), how sheltered from the rain (12 rays up, in a
cone of 20 degrees, in front of its face), how much of an exposed edge it is
on (the generator labels these: the timbers' chamfers and ends, the corners
of masonry walls and of the base, each a narrow border round a flat middle)
and what kind of surface it is (wood, plaster, stone, roof). The walls, the
timber, the roof and the base are `OBJECT_MATERIAL_AGED`: the building
shader ages them by these and by the world (`age`, `age_random`,
`dirt_meters` in objects.ini). For the values to have somewhere to live,
the big flat faces (plaster panels, slopes, the base) are cut into cells of
about 0.5 m at the full level only; the coarser levels copy their nearest
full vertex. The glass and the collision shell carry none.
`house_view --show sky|edge|shelter` draws a channel as gray; `--age A` and
`--dirt M` draw the house older or newer.

**The details atlas** (`tools/house_atlas.c`; `data/props/textures/house_details_{color,normal,arm,emissive}.png`,
a 2048 square): the small painted things a house wears, each in a cell of
its own: eight kinds of window glass (diamond and square leaded, a wooden
cross of four panes, crown glass roundels, grimy, boarded up, barred), four
shutters (plain, with a heart cut out, louvred, braced), door leaves, the
ends of joists and carvings, ornaments with a shape (a lantern, a flower
box, a number plaque, a sundial, a horseshoe, a hook with rope) and eight
hanging signs (inn, smith, baker, tailor, herbalist, boar, a blank board,
one free). A house picks its glass, its shutters and its joist ends by its
seed, grimy or boarded lights now and then on a worn house, a horseshoe
over the door or a plaque beside it on some, and an inn or a craftsman
hangs a sign out over the street (`sign`: -1 by the kind, 0 none, or a
cell's number). The atlas material is alpha tested (the shapes), its glass
glows after dark through the emissive map. What is there now is a stand-in
painted from nothing; `potop_house --atlas DIR` writes the template with
the cells framed and numbered (`house_details_template.png`) and the
painter's guide (`house_details_atlas.md`: each cell's place, size in
meters and what goes in it, and a prompt to start from) for a painted one
to replace it, cell for cell.

A timber framed wall is read as bays between posts (`post_spacing`), with
a sill beam, a top plate and corner posts heavier than the rest (the
timbers' sizes are a hierarchy from `post_width`: corner posts, posts,
plate, rails, studs). Some bays get a **band of windows** filling them
between the posts (`window_lights` lights of `light_width` between
mullions, a transom when tall, the glass set back, a sill board, now and
then a flower box), more upstairs and on the front, fewer at the back and
below; the door's bay (off the middle of the front) gets the door, a
heavy frame and a plank door with battens; the end bays get a brace from
the corner post's foot to the plate, bowed when `figures` is on; the
blank bays get St Andrew's crosses, pairs of braces or studs
(`figures`), or close studding (`studding`). The plaster between is set
back (`panel_recess`) and, with `plaster_tint`, colored by the seed (the
timber towns' salmon, sage, ochre, blue, cream). Each upper floor stands
out over the one below by `jetty`: the joists' ends and the bressummer
show, and where it stands far out (`porch`) posts hold it up. The gable
has a king post, a collar, studs, braces and a small window; the purlins'
ends and the ridge beam's show under the roof's edge.

A stone wall is coursed stone with single windows (`window_width`) deep
in it, with shutters, timber lintels over the openings and quoins up the
corners. The roof (`roof`) is a `gable`, a `hip` (four slopes) or a
`half_hip` (the gables cut `hip_fraction` of the way down under a small
slope, a plate along the cut): slopes that **sag** along the ridge and
bow across with age (`roof_sag`, `wear`), flatten up at the eaves
(`eave_kick`), ripple a little; a cap of ridge tiles rolled down onto
both slopes, following the sag; barge boards and a fascia; the rafters'
feet
under the eaves; **dormers** (`dormers` on each slope at most, how many
the seed's; gabled or shed ones, `dormer_kind`, with a window, their
cheeks boarded); the gable standing out over the top floor on the
attic's joists, with brackets (`gable_jetty`); `thatch` instead of tiles:
nearly three times as thick, its eaves and verges rolled round, a fat
roll along the ridge, no boards and no rafters' feet; a stout stone
chimney through the back slope with a band and a cap. `balcony` hangs a
plank balcony on brackets on the top floor's front, with a railing of
posts and balusters, roofed with boards on two posts; `oriel` hangs a
bay window on brackets on the first floor's front, three sides of
lights under a little hipped roof; `annex` leans a
one floor annex against the right side, its walls in the house's style,
under a single slope. `wear` (0..1) also leans the timbers and unsettles
the panels, twists every timber a little round its length, and sags the
long beams in their middle.

The house stands at y = 0, its middle over the origin, its door looking
down +Z, as a layout's front. Six materials, by what they are made of:
`walls` (plaster or stone), `timber`, `roof`, `base` (the base's stone,
a masonry ground floor's, the chimney's, the quoins), `glass` (dark, a
little emissive: the windows glow after dark) and `collision`, a shell
of about 50 triangles a little inside the real surfaces, flagged
`OBJECT_MATERIAL_COLLISION_ONLY` (512; to be read by the engine: solid,
never drawn; until then `shape = mesh` takes the whole thing and the
shell stays hidden inside). Walls, roof and base are `WEATHERED` with
texture coordinates, not triplanar (a triplanar material would cost the
model its impostor): the game grows moss on their north sides and lays
snow on the roof in the snow's months. Textures: four files per material
under `data/`, named in the recipe (`walls_color_texture`,
`walls_normal_texture`, `walls_arm_texture`, `walls_emissive_texture`,
and so for `timber`, `roof`, `base`, `glass`; `walls_meters` and the
like one repeat), shared by reference between every house that names
them; none named: the style's stand-ins
(`data/props/textures/house_plaster_*.png`, `house_timber_*`,
`house_roof_tile_*`, `house_base_stone_*`, `house_wall_stone_*`), made
once, tiling by construction: lime plaster with hairline cracks and
streaks, old oak along the grain with knots, beaver tail clay tiles each
lying over the one below, stone in courses with worn edges.

Three levels of detail, meshes of their own after each material's own
indices (`OBJECT_MATERIAL_LOD_MESHES`), by construction rather than by
simplification: the first without the chamfers and the small pieces
(mullions, sills, shutters, battens, joists' ends, rafters' feet,
balusters), the second the frame's big timbers over flat panels and a
coarse roof, the third a box under a roof; a town house is about 11 000
triangles, then 4 400, 1 000, 200. `lod_errors` in the header is the
biggest thing each level leaves out (meters). `triangles` in the recipe
is only a hint for now.

`tools/house_view` draws a house as the game does, headless, into PNG
files (the same recipe arguments, then `--out PREFIX`, `--views
front,back,left,right,above,close,street` or `all`, `--month M` for the
snow, `--wet W`, `--detail N` for a level of detail, `--collision` to see
the shell, `--sun AZIMUTH,ELEVATION`, `--size WxH`): on a machine without
a screen, `SDL_VIDEO_DRIVER=offscreen` and Mesa's lavapipe are enough. The
weathered materials go through the prop shaders, the rest through the
object shaders, as in the game. Interiors are not made (the house is a
shell); the plan stays in the recipe for small inside maps later. The
door is part of the house for now; a door of its own, hung at the door
anchor, is the next step, with the hip roof, oriels and thatch.
