# The building generator (tools/house_grow.c)

One file, about 17 700 lines, writes .pobj directly. Recipes are
house_plans/*.house (key = value); `potop_house --list` prints every key
with its meaning. `<material>_color_texture`, `_normal_texture`,
`_arm_texture`, `_emissive_texture`, `<material>_meters` work for every
material by its name.

## Materials (house_grow.h, house_material_kind_t, in order)

walls, timber, roof, base, glass (the dark inside), details (the atlas,
alpha tested), collision (never drawn), fringe (thatch edge), roof_timber,
log, ivy, creeper, iron, embers, leather, hood, chimney, floor, ceiling,
carved, footing, stone, millstone.

- A file holds at most 16 materials (OBJECT_FILE_MAX_MATERIALS); a house
  writes only those it uses.
- `base` is masonry (a house's base, roundhouse, watermill wall, piers).
  `stone` is loose or cut stone (blocks, rubble, ore, quoins, sills, steps,
  padstones of yards and mills): limestone unless named, 1.2 m. `millstone`
  is hard stone (millstones, grindstones): granite unless named, 1.0 m.
  Both added in step 34b.
- `footing` takes the base's textures unless named (church).
- Weathering kind per material (weathering_of): stone for base, chimney,
  stone, millstone; plaster for footing; wood for the rest (the church's
  walls weather as wood).

## Levels

LEVEL_FULL 1, LEVEL_MEDIUM 2, LEVEL_LOW 4, LEVEL_BOX 8, LEVEL_ALL 15. Every
piece says which levels it is in. lod_errors come from builder->dropped[]
(what each level leaves out); for church, mill, watermill and workshops a
floor of {0.09, 0.25, 0.9} m is applied at the end of the grow, else the
engine switched levels at 20 m (their dropped pieces were only bevels).
Open sheds: inner wall faces, gables and the roof's underside are
LEVEL_ALL, or far levels are see-through.

`interior = 1` in a recipe turns the hidden-surface removal off (sails,
wheel, workshops).

## Special buildings (recipe keys)

- `church`: log church with porch (porch_floor_oak floor, porch_frieze_boards
  over the arches, leaded diamond windows), bell tower with gallery, rope
  mitred round the corners, footing mitred. Finished with Radu.
- `mill` 1 body, 2 sails, 3 both; `mill_height`, `sail_length`,
  `sail_cloth`, `weatherboard_lip` (0: the boards are in the texture),
  `mill_roundhouse`. Post mill on a trestle, roundhouse of 16 sides.
  Sails are a separate model (windmill_sails.house, mill 2, origin at the
  hub) spun by the engine; objects.ini type `windmill_` next to `windmill`.
- `watermill` 1, 2, 3; `watermill_stone_height` 2.7, `wheel_radius` 2.1,
  `wheel_width` 1.0. Undershot wheel, hub at (3.45, 2.22, 0) in model
  meters, for now in the air (no river). Wheel model watermill_wheel.house.
- `workshop` 1..9: smelter, lumber_yard, carpenter, stonecutter,
  quarry_works, charcoal_burner, palisade, watchtower, palisade_gate.
  Built from yard_* helpers (yard_log, yard_log_pile, yard_plank_stack,
  yard_block(_stack), yard_rubble, yard_heap, yard_sawhorse, yard_bench,
  yard_chopping_block, yard_barrel, yard_trough, yard_shed, yard_cart_wheel,
  yard_shear_legs, yard_scaffold, yard_treadwheel_crane,
  yard_palisade_run, yard_tower). The watchtower's ladder goes up inside
  through a hatch in the middle of the platform (34b).
- Smithy furniture: `smithy_furniture = 1` with `smithy = 1` calls
  `smithy_works` (hearth, anvil on a stump, quench trough, racks; 34v).
- Charcoal burner (workshop 6, 34w/34x) is `charcoal_clamp`, built from
  Radu's reference photo: a superellipse dome (`clamp_radius_at`) of soot
  cover (FLOOR material, no moss), base log ring, a mid ring on slanted
  stakes, a ladder, a mouth with a lip and a dark shaft with embers at the
  top (CLAMP_MOUTH), smoke from the mouth. `charcoal_open = 1`
  (charcoal_burner_open.house) is the burnt-out clamp being raked open:
  cover only round the back (CLAMP_OPEN_START..END), a mound of charcoal
  lumps inside, a raked-out heap with two rakes, no ladder. Since 34y the
  opened one is sunk (charcoal is a third of the wood): the cover a shell
  14 cm thick round the back, 1 m high, the heap 0.85 m of 1800 lumps.
- Smelter (workshop 1, 34y, from Radu's reference picture): the bloomery
  stands back under the ridge, drawn at a man's size then scaled 1.2 round
  its foot (`scale_made_since`: vertices and effects made since a mark).
  Its foot is courses of `fieldstone`s (rounded lumps, between a ball and a
  box, a superellipsoid with bumps; it flips its winding for a left handed
  frame) bedded in clay (`bloomery_radius`, `bloomery_arch_bulge`), an arch
  of fieldstones round the tapping mouth, the mouth's sides dark faces from
  the clay to the shaft, coal lumps glowing inside. Its soot comes from its
  top (`builder->soot_from_top`, unlike the smithy hood's from its mouth
  down). STONE there is granite (smelter.house). Over it a clay hood
  (`bloomery_hood`) hung on iron rods, its flue up through the roof, the
  smoke and sparks out of its top; the furnace at x 1.3, mid bay, clear of
  the shed's tie beams (x 0, 2.6) and the back purlin (34z).
- Trade yards (34z): CARVED (the fresh oak) weathers as plaster when
  `workshop` is set (no silvering in the sky: fresh planks); plank stacks
  are CARVED, their bearers and stickers old wood. Log piles: each log
  rests on the two under it at its real contact height. The palisade's
  earth bank is gone.
- Palisade stakes (35a) are `palisade_stake`: truly round (`house->logs_round`
  turns yard_log's squash off), the cone as many sides as the log and into
  its top. The gate's towers are shut by stakes OUTSIDE them (35b: in
  front of the front posts and braces, and along the outer sides from the
  wall), no rails (they would cross the braces); the ladders stay inside.
- Watermill openings (35a): jambs, lintel and sills 1.5 cm over the
  reveal's edges (flush, they z-fought the reveals).
- Opened clamp: its foot logs out clear of the cover shell, none reaching
  into the open side.
- yard_log end grain: each end turned, maybe mirrored, its heart moved,
  from a hash of its position (not the random numbers: nothing else moves).

## Checking a building

Render with house_view from several sides and close up; look for sky through
walls (holes), z-fighting (coplanar faces of different materials: offset
one by a few mm or cut it), slivers (thin ends of logs or boards poking
out), floating pieces at far levels (a piece in LOW but its support only in
FULL). Use `--detail 1..3` to see each level.

## Watermill and the river (later)

Discussed, not done: race depth from the river, rpm driven by the flow, a
flood breaking the wheel. Wet mossy planks texture for the wheel
(wet_mossy_planks) and log_bark_full were offered as texture prompts for
Radu to make.

## Farm buildings (35c)

`workshop` 10 hen_house, 11 pigsty, 12 corn_crib (`crib_size` 2:
corn_crib_large, 12 by 4 m, a landing, three hatches, doors both ends).
Built from the hut helpers: `hut_wall` (boards, outside and in),
`hut_gable` (a gable's triangle), `hut_slope` (one slope of a roof, with
its underside and edges; the top edge for a lean-to), `hut_gable_roof`,
`hut_opening` (a dark hole with a narrow frame), `hut_door`, and fences:
`picket_fence` (bark poles, the hen run's), `picket_gate`, `rail_fence`
(split rails on posts, the pig pens'), `hen_feeder`. Sizes are a farm's
(Radu: "the EU must not be cross"): the hen house 3.2 by 2.1 m, a man walks
in, its run 8.4 by 9 m; the pigsty 5.2 by 2.7 m, two pens 4.4 by 8 m. The
crib's corn is FLOOR with ochre plaster, a stand-in until a corn texture.

## Wells (35i)

`workshop` 13 well_sweep (cumpana: `well_curb`, a forked post, the sweep
on an iron pin, stones lashed to its end, a pole with `well_bucket`, a
trough), 14 well_winch (the curb, two hewn posts with a ridge beam and a
shingle roof of two `hut_slope`s, a roller log through a plank drum with
iron-tyred flanges, the chain wound on it as a helix of `chain_link`s
(a plain coil at the far levels), the chain hanging from its front to the
bucket, an iron crank: shaft, flat bar arm with bosses, iron grip).
`well_curb` (35i, Radu: "not as a joke"): its face FOOTING with the
church footing's drystone, a fine grid (footing_cell) pushed out by the
texture's _height picture (`church_footing_height_load/at`, loaded and
freed by the curb if nobody else has it), the picture wrapped a whole
number of times round it (no seam); inside the stone down to 0.35, the
shaft dark below, a dark disc at 0.03 (the terrain would show through
anything lower); a coping of 13 granite `fieldstone`s in MILLSTONE.
`millstone_triplanar = 1` (35i) makes MILLSTONE triplanar
(OBJECT_MATERIAL_TRIPLANAR, triplanar_meters = millstone_meters): the
engine's building pipeline now honours the flag (building_material_uniforms,
compare_building_materials splits on it), and a building with triplanar
stone keeps its impostor (impostor_worth_one: only models without a
weathering buffer, the props, lose it; the bake uses the UVs).
house_preview does the same. Recipes: field stone at 0.8 m (the sweep's
weights), drystone footing at 2.3 m, granite at 1 m, the posts old split
wood (log material), no bark.
Crib (35i): door frames (posts either side, a head) at its door ends.
Hen run: its feeders' boards lean out (a V), the water a shallow pan.

## Log cabin (35j)

`workshop` 15, log_cabin.house (Radu: a long body 10 m, a shorter one 5 m
out of its middle, the ridges as high; the wing a fifth narrower than the
main is deep, so steeper: `wing_slope`; the church's dark shingles at 7 m):
a T of round logs (`yard_log` with
`logs_round`, LOG material = house_timber, peeled brown). `cabin_t`
(cabin_of) holds the plan: main block 10 by 6 m (log centers), ridge along
X; the wing 4.8 m wide out of its front to z 8, ridge along Z, the door in
its gable. Corner ends come out no further than the roof over them lets
them (end_eave_y, end_slope: under an eave the slope comes down). Courses 0.26 m (logs 0.3 m thick overlap: the saddle notch is
just the crossing logs interpenetrating); walls along Z half a course up
from those along X (`cabin_course_y`), their first course sunk in the
footing and flush. `cabin_wall`: courses full up to its eave (ends 0.3 m
out past the corners), then on a gable wall shorter under the roof;
openings cut the logs whose middles fall in their asked range,
`cabin_opening_frame` snaps head and sill to the logs over and under (head
up to the next log's middle). The wing's slopes run back into the main's
front slope (the valley: where the two planes meet; the main slope is not
cut, it is under the wing's there); `cabin_slope` lays the shingles by the
slope's own axes. Purlins through the gables (the main's ridge pole stops
at the chimney). Footing: `church_footing_surface` round the T (reach back
by `out` at the two inside corners). Chimney: two blocks of the same
surface out of the left gable (`cabin_chimney_block`), smoke. Porch in the
right corner: oak deck, three posts on granite padstones, a lean-to under
the main eave. About 35k triangles at full, 23k of them the stone faces
(footing_cell 0.06).
Recipe keys (35k): `cabin_length` (the main block, 0: 10 m), `cabin_no_porch`.
log_cabin_long.house: 12 m, no porch. The chimney's stack has its back face
too (it shows over the roof).
Stone cabin (35l, stone_cabin.house, `cabin_stone = 1`): the same T, its
walls dry laid river stone 0.56 m thick round the logs' lines
(`cabin_stone_walls`): one displaced surface per face of the outline
(`cabin_stone_face`: BASE material = drystone at 3.4 m, the FOOTING's
height picture, a 9 cm grid): one grid per face, its lines on the holes'
edges (window and lintel rectangles), the holes' cells skipped
(`smooth_grid_holes`; separate pieces per strip made T-junction cracks,
35m), flat toward the ends and the holes' edges; eave walls'
tops under the roof at their outer face, gable walls' tops following the
roof toward the corners (`cabin_face_top_t`); a ledge back over the top.
Openings (`cabin_stone_opening`): reveals 0.2 m deep, granite sill or
threshold, an oak lintel through the wall, frame, glass or door set back.
Wall plates (logs) on the eave walls; gables logs (`cabin_wall` skips its
full courses, slabs and frames when stone). No footing, no corner ends.
About 54k triangles at full (37k the walls, 11k the chimney).
Stone faces' edges (35n): T-junctions where a face's many edge vertices
meet a reveal's single edge, or another face's at a corner, sparkled
cracks in the game. Now the vertices on a hole's edge and at an outer
corner are tucked 12 mm in behind the reveal / the other face, and at the
T's crook the faces run on 12 mm into each other.

## Cow farm (35o)

`workshop` 16, cow_farm.house: a byre 14 by 7 m (log lines) at z -7.5, its
walls a meter of dry laid stone (`cabin_stone_face` round the logs' lines,
door holes through it, no ledge over a hole reaching the top) and the
cabin's logs over it (a `cabin_t` with base_top 1, door_bottom at the
ground, center_z: a wall along X faces out away from it); half doors
(`cabin_opening_t.half_door`: lower leaf shut, upper swung flat to the
wall), small windows, a hayloft hatch (`hatch`: no glass) in the right
gable with hay out over its sill and a pulley on a beam out of the apex.
Thatched roof (`thatch = 1`, cabin_slope 0.28 thick, a ridge roll); the
hay (rick `farm_hay_rick`, rack `farm_hay_rack`, the hatch's) is ROOF too.
Yard 20 by 15 m of `rail_fence`, `farm_gate` (two board leaves), trough,
bucket, stool. The dung heap (35p, Radu's reference: against the wall by a side door):
`farm_dung_heap`, a lumpy cone (polar grid, three octaves of clods) pressed
flat to the left gable's stone beside its back door (an open door:
`cabin_opening_t.open`, the leaf flat to the wall), FLOOR = Radu's dung
texture (props/textures/dung_*, ARM made: glossier where wet and dark); a
fork leaning on the wall, a board of pegs with a bucket and a coil of rope.
No anchors (Radu: it is fenced). About 28k triangles.

## Shepherds' fold (35o)

`workshop` 17, stana.house: a log hut 4.8 by 3.8 m (a `cabin_t`: logs 0.13,
courses 0.22, base_top 0.3) on granite field stones, a 52 degree roof of
dark shingles, smoke from its ridge, its door to the fire (`stana_fire`:
a ring of stones, embers, three poles, the cauldron on a chain); a split
log bench, barrels (churns), firewood. The pen (x 4.2 .. 16.2, z +-4.5) of
`wattle_fence` (stakes every 0.5 m, rods 46 mm apart woven in front of one
stake, behind the next; far a panel), the strunga (`stana_strunga`: four
posts, a little shingled roof, the narrow way, the milker's stool, a
pail), a trough. About 55k triangles, most of them the wattle's rods.
Gable logs (35p, all the T cabins, the byre, the hut): `cabin_gable_log`,
round, their outer ends cut slanted along the roof's underside (each end
point where the roof comes down to it): no stepped gaps under the roof;
square cuts round a hole. The stana's cauldron is a lathed bowl
(`smooth_grid` outside and in, a rim, a dark disc): the plain cylinders
had no inside faces.
Hay (35p): Radu's hay texture (props/textures/hay_*, resized to 1024, ARM
made: AO from the blurred color, roughness about 0.93) on CEILING in the
cow farm (rick, rack, the hayloft's), weathering as a roof there (the rain's
streaks down upright faces now skip the roof kind, prop_body.glsl).
`floor_triplanar`, `ceiling_triplanar` (35p) as `millstone_triplanar`: the
cow farm's heap (FLOOR) and hay (CEILING) laid on from the sides.
35q: the cow farm's hay (CEILING) is not aged (OBJECT_MATERIAL_AGED off):
drawn by the props' pipeline as triplanar, no weathering (Radu: the aging
on hay looked wrong). The back door is shut (open, its dark looked bad).
35s: stone faces' rows on one lattice from the ground (cabin_grid_lines
lattice mode) plus every hole's heights of the house (cabin.stone_cuts_y),
cut off under a gable's slope instead of squeezed: two faces' rows meet
one for one at an outer corner, both flat to its line (the 35n tuck there
left a groove along the corner). Byre doors: `door_forward` puts the leaf
at the stone's face (sunk 23 cm in its hole the lower half was dark), the
holes as wide as the jambs' outsides (the back door's was 5 cm wider: a
gap into the wall). Cabin and byre footprints tight to the walls: the
ground clears grass in every cell an outline touches (ground_footprints.c).
Small rubble casts no shadow: near shadow map 4096 over 100 m (about 5 cm a
texel) and the depth bias are bigger than a 5..15 cm stone; not the model.

36t: the stana's spots for the flock (game/animals.c): "yard" the pasture
before it (6, 20, half 14 x 12, off the pen), "fold_approach" (x0 - 1,
z1 + 2: in front of the gap between the hut and the pen) linked to
"fold_gate_out" (x0 - 1.2) linked to "fold_gate_in" (x0 + 1.4), both at
gate_z - 0.04 (between the strunga's narrow posts, clear of its pail),
and "fold" the pen inside short of the trough (x0 + 1.6 .. x1 - 2.2,
z +-(z1 - 0.8)). The fences' collision boxes stop them; the trough,
barrels and fire have none (not on their ways).

## Workshop windows in emissive groups (36g)

cell_quad: a window cell (LEADED_DIAMOND..BARRED, not BOARDED) laid with no
group set (the workshops' own windows: cabins, stone cabin, byre, church,
mills; glass() sets its own) gets one from window_group_at: the group
already made within 0.45 m (the coarse levels' glass stands deeper), else a
new "window" record (floor and side -1). So the game lights them by the
hour (objects_light_windows) and lights.c gives them window lights.
potop_house prints "N windows lit by the game". Regenerated: church (+2
variants), cow_farm, log_cabin, log_cabin_long, stone_cabin, watermill,
windmill.

36p: every window record also carries the way it faces out
(record_facing: OBJECT_EMISSIVE_FACING plus the yaw byte in the flags),
from face->out in glass() and from the first quad's normal (quad_edged's
winding: cross(p2 - p0, p3 - p1)) in window_group_at. Every house with
windows regenerated.

36q: annex() turned or mirrored the annex's vertices (annex_side 3 the
left, 2 the back) but not its windows' emissive records: their center,
facing and side stayed on the right, so the game lit them and laid their
light where no annex stood. Now moved with it (annex_place on the center
and the facing, side remapped). Regenerated: stone_house, cottage (+2
variants each; the recipes with annex = 1).

## Spots (36h)

spot(house, name, at, yaw_degrees, half_x, half_z) and spot_link(a, b)
write the creatures' places into the .pobj (version 12). The hen house:
"yard" (the run clear of the ramps), "peck" at the three feeders, "drink"
at the water pan, per hole "hen_ramp_foot" - "hen_hole" - "hen_roost",
linked yard-foot-hole-roost. Next: the smithy's anvil/forge/bellows spots,
and the other workshops, for the trades.

36n: cow_farm spots (yard, hay x4, trough x2, byre_door_out/in, byre_stall x6); the right byre door open (the cows go in by it).

## The apiary (prisacă, workshop 18, 36w)

house_plans/apiary.house -> objects/apiary.pobj. workshop_apiary: 11 box
hives facing +Z in two rows (back z -2, six 1.8 m apart; front z 0.9,
five between them), each apiary_hive(foot, front, style, wood, super):
two sleepers, a body (0.5 x 0.44 x 0.54 m: board_box, six flat faces, the
boards level round it; NOT box(), which is for timbers: its ends come from
the details atlas as beam-end rings and its sides drawn in by the bevel,
Radu saw rings and missing faces), half of them a
super on it, the lid by style (0 a little shingled gable, 1 a board
flat on it under a 3 mm sheet of iron, its edges folded 4 cm down: a
tilted sheet left a gap to see through, a 5 cm one looked like armour;
2 boards and a field stone), its wood by wood (0 the walls'
house_board_wall, 1 the carved wood's porch_floor_oak, both upright boards
in the picture turned a quarter; 2 the roof timber's mill_weatherboard,
level as it is; old_wood and wood are atlases, a band of end rings at the
bottom: never on a flat face), the
entrance's dark slot (HOUSE_MATERIAL_GLASS) and its landing board, a
"bees" effect before it (particles.ini [bees], given to Radu: dark specks,
alpha, turbulent). Two log hives (apiary_log_hive: a hollow trunk on a flat
stone, a slab on it, its hole) at the back corners; a wattle fence round
(x +-6.5, z -4..4.5), the gap in front x +-0.7 between two posts; a bench
(a board on two stumps) by the gap with the smoker on it. Spots: "hive"
behind each (where a beekeeper works it), "bench"; workshop_work anchor
at the bench. Footprint: only a trodden path in from the gap (at most 4
footprints a model: none per hive; the grass grows round them, hidden
inside the boxes). Collision: only the fence boxes (the hives are solid
as drawn: whole-row boxes stopped the player between them).

36z: the lids' undersides were missing because hidden_left_out (recipe
interior 0) judges from views at a man's height and dropped them (only
seen from low in the grass, as Radu looks): apiary.house has interior = 1.
Remember it for any small low thing seen close (a prop-sized building).
The super now sits on the body (a 3 mm slit through the hive showed the
sky); the boards lid's boards edge to edge (their gaps showed the sky);
the iron lid a 2 cm board and a 2 mm sheet over it, no fold.
potop_app.c logs each --houses object's place ("Houses: 'X' at (x, z),
turned a half turn"): model (x, z) is world (X - x, Z - z) there, for
placing a test camera.

37a: the gable lid closed: its gables are pentagons up to the slabs'
undersides (the slabs' foot line 2 cm over the ridge, falling rise over
run), with strips along the sides from the body's top up to the eaves (a
5 cm gap under the eaves before). Wattle rods: builder.cylinder_uv (added
to mill_cylinder's texture coordinates) set at random per rod, and
cylinder_uv_swapped (U along the cylinder: the bark's grain runs level in
its picture, so a rod showed rings round it, all alike). Regenerated:
apiary, stana (the only wattle users).

37b: the gable lid's floor board gone (its sides were in the planes of the
side strips and the gables: z-fighting at the top of the body; the body's
own top closes it). The hives' and log hives' stones are the hard stone
(HOUSE_MATERIAL_MILLSTONE, apiary.house millstone_triplanar = 1, granite):
triplanar, no seams.

## Modules from recipes (37f)

A recipe with `module = NAME` makes a module too (engine/module_format.h):
potop_house writes the body (the output .pobj), each other part as
data/objects/NAME_PART.pobj and data/modules/NAME.pmod, prints the dump and
the objects.ini entry. The building code declares parts and states while it
builds (house_grow.c: module_part, module_state, module_pose,
module_spinning); each part's recipe string: `@N` the Nth file of
`module_recipes` (next to the recipe), then `;key=value` settings over it
(or over the body's recipe without `@`). Parts grown the same share one
model (the rope's lengths). `module_piece = N` builds only that moving
piece with its pivot at the origin (no footprint, no collision).

- windmill: sails (`@1;mill=2`: windmill_sails.house) at the hub, axis
  (0, sin 8°, cos 8°), 9 rpm; states working (first) / still.
- watermill: wheel (`@1;watermill=2`) at the hub, axis X, -6 rpm.
- quarry_works: crane wheel (piece 1, axis X), rope1..4 (piece 2, each
  (4.2 + 2.25) / 4 m, hanging from the jib head), load (piece 3: block and
  hook). States down (load on the ground, hold 4 s, then up) and up (wheel
  +716° = the drop over the windlass radius 0.18, hold 3 s, then down),
  10 s each way. Rope lengths: down each below the last; up the ones that
  no longer fit end at the hook, over the last.
- 37g/37h: no waiting block where the load comes down (under the jib
  head, x 0.8, z 3.62 in the model); a pulley at the jib head (wheel r 0.12
  between two cheeks); a pulley on the mast's side 0.2 m out, 0.3 m under
  its top (wheel r 0.13, pin through the mast, a band round it): the rope
  from the windlass (x half_width + 0.8) round its front, on to the head.
  (37g's .pobj files were stale: potop_house had not been rebuilt, make
  needs PKG_CONFIG_PATH even for the tools.)
- smithy (37j): `module = smithy` (at the recipe's end: a preset line
  resets everything before it). Pieces: 1 the bellows' top board with the
  leather's upper half (pivot the nozzle end, axis cross(along, up), open
  11.5 degrees: its far end up 0.22 m; the upper leather reaches on down
  inside by as much), 2 the pole (pivot the post's top, its short end over
  the board's far end, the handle 0.85 m out the other way at 1.80 m;
  turned just so far that its short end rises as much as the board's far
  end, 26.7 degrees, the handle down ~0.38 m), 3 the rope (37v: its own
  piece, only slid up by that rise, 0.216 m, so it hangs straight; as part
  of the pole it swung off the board).
  States still / pump_down / pump_up. Spots for the smith: smith_forge
  (back + 1.38, facing the hearth), smith_bellows (0.4 m beyond the
  handle, facing the post), smith_anvil (between hearth and anvil, facing
  it, 0.07 m to its left: the clip strikes at x -0.07), smith_trough (the
  trough 0.48 m to his left, 0.43 ahead), smith_rest (unused yet).

37w: door spots. door_spot(house, foot, out): a spot 0.6 m out of a
ground floor door, facing it: `door` (dwellings: no workshop, or workshop
15, the cabins), `inn_door` (kind inn), `barn_door` (kind barn),
`work_door` (other workshops: the cow farm's). From door() (floor 0) and
the cabins' doors (log and stone), not from hut_door (hens, pigs).
Regenerated: barn, cottage, cow_farm, craftsman, gallery_house, hip_house,
hovel, hovel_log, hovel_rubble, inn, log_cabin, log_cabin_long,
stone_cabin, stone_house, tall_house, thatched_cottage, town_house (with
variants). (`potop_house --all house_plans` also rewrites church,
charcoal burners, smelter, wells with different bytes but the same size:
not shipped, left as they were.)

37z: door_way(house, face, along0, along1) for ground floor doors (from
door()): a floor more than 0.25 m over the ground gets stone stairs
(rises of at most 0.18 m, treads 0.3 m, the door's width + 0.3) down from
the porch's edge (front_setback) or past the door's own step (0.4 m out,
made solid to the physics), each step drawn in the base's stone and a
collision box; the door's spot at their foot (y 0), "<name>_sill" at the
top 0.35 m before the door. Lower floors: the spot 0.6 m out as before.
Cabins' doors: door_spot as before (no stairs). Regenerated the 13 framed
houses with doors (barn, cottage, craftsman, gallery_house, hip_house,
hovel*, inn, stone_house, tall_house, thatched_cottage, town_house).

