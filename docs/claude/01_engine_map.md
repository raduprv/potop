# Engine map

About 142 000 lines of C (November 2026). docs/manual/architecture.md ("How it fits
together" lists the older modules; this file adds the rest and the order of
a frame. Every header starts with a paragraph saying what the module is:
read the header first, then the .c.

## Directories

- `engine/` (103 .c): the engine, built into build/libpotop.a.
- `game/`: the game on top (main.c with the command line, game.c, bear.c,
  martin.c = Moș Martin, hunt.c, places.c, debug.c). Talks to the engine
  through `engine/potop.h` (potop_start, potop_frame_begin/end, potop_stop,
  queries about actors, containers, the player...).
- `editor/`: potop_editor (main.c 6 300 lines, ui.c, tools.c, inspector.c),
  same engine, edit mode.
- `tools/`: offline generators and converters: potop_house + house_view +
  house_editor (house_grow.c), potop_plant + plant_editor (plant_grow.c),
  potop_prop + prop_editor (prop_grow.c), potop_convert (glTF, e3d),
  potop_texture (procedural textures), cloud_bake (VDB clouds, vdb_read.c).
- `data/`: ini files (settings, objects, materials, layouts, actors, items,
  weapons, ammo, explosives, impacts, particles, soils, ui), shaders
  (GLSL in data/shaders, SPIR-V in data/shaders/spirv, rebuilt by make),
  objects (.pobj, .e3d), props/textures, cache (texture_cache BC7,
  impostors).
- `house_plans/`, `plant_plans/`, `props_plan/`: recipes for the generators.
- `third_party/`: Box3D, Nuklear, miniz, stb, cgltf...

## Modules the manual does not list

- World: `world.c` (the struct), `world_create.c` (init, land, populate,
  destroy), `world_frame.c` (begin_frame, update_actors, simulate,
  end_frame), `world_render.c` (the frame's drawing, below),
  `world_settings.c` (settings applied, regrow).
- App: `potop_app.c` (the window, GPU, settings, world, player; the
  `--houses` gallery; POTOP_SCRIPT), `potop.c` (the game's API).
- Actors and AI: `actors.c` (3 000 lines), `actor_model.c` (.pact,
  skinning), `navigation.c` (3 300: 1 m grid with surfaces, regions, path
  searches), `senses.c` (sight), `noises.c` (hearing), `light_probe.c`
  (how lit a place is: stealth).
- Player and game support: `session.c`, `items.c`, `ground_items.c`,
  `weapons.c`, `explosives.c`, `damage.c`, `journal.c`, `markers.c`,
  `areas.c` (trigger rectangles), `hud.c`.
- Rendering extras: `sky_renderer.c`, `clouds.c` + `cloud_weather.c` +
  `cloud_paint.c` (volumetric clouds: atlas of real VDB shapes stamped on
  a weather map; the player can paint the sky), `fog.c` (valley fog,
  froxels: fog_inject/integrate/rays), `grass.c` (blades made on the GPU
  each frame), `rain.c` (rain, splashes, rainbow), `lights.c` (hundreds of
  point lights, no shadows), `impostors.c` (far objects as pictures,
  baked at load, cached), `hiz.c` (depth pyramid for occlusion culling),
  `sky_occlusion.c` (baked sky visibility of the ground and the air),
  `ambient_occlusion.c` (GTAO from the scene's depth, the small scale),
  `object_effects.c` (particle effects written into models: forge fire,
  chimney smoke), `ground_footprints.c` (buildings mark the material map),
  `waterfalls.c`, `texture_cache.c`, `scene_ground.c`.
- Tools in game: `edit.c` (brushes, plant/clear, layouts), `inspector.c`,
  `console.c` + `console_commands.c` + `console_window.c`.
- Profiling: `profile.c` (simulation phases). The frame profiler is
  frame_profile_* with the FRAME_* sections (in a header grep FRAME_POLL);
  the console command `profile` prints ms per section since the last call,
  `profile frame` one frame step by step. Ask Radu for these numbers
  before guessing at performance.

## A frame (world_render.c, render_frame)

1. Chunk bounds of changed terrain (`terrain_update_chunk_bounds`, only
   dirty chunks, OpenMP only when many).
2. Objects follow the ground (dirty chunks only, budgeted per frame) and
   floating objects follow the water.
3. Navigation follows the map (blocker areas: locked gates).
4. Flood/silt takes the cover (material map).
5. Sky occlusion worked out a few directions a frame when asked or missing.
6. Far shadow map: only when the light turns or (in the editor) the ground
   changes; at most every shadow_update_interval_seconds.
7. Near shadow cascade around the camera's target, two layers: the still
   one (terrain, standing objects) redrawn when the target moved a sixth of
   its square or something changed; the moving one (loose objects, actors,
   spinning parts with FLAG_SPINS) drawn over a copy of it when something
   moves.
8. Scene uniforms (camera, sun, fog, shadows, clouds, under water).
9. Hi-Z from the previous depth + objects culled on the GPU
   (objects_cull.comp: modes clear/count/offsets/scatter, indirect draws).
10. The scene pass (linear light, half floats, render_scale): terrain,
    objects (props, buildings, plants, impostors), grass, actors, sky.
11. Water (reflections at half size in passes of their own).
12. Volumetric clouds composited, valley fog over it all.
13. Particles, rain, snow, light cores.
14. Post: exposure (exposure.comp), bloom, TAA, tone mapping into the
    swapchain; then the UI (Nuklear) and present.

The simulation (water, erosion, wetting, physics with Box3D, particles) runs
in world_simulate at a fixed 30 Hz step, before the drawing.

## Objects (objects.c, 4 100 lines)

- Instances in a grid of 32 m cells, counting-sorted by (cell, type) when
  something is added or removed (`grid_dirty`); `objects_touch` only marks
  the GPU slot for re-upload.
- GPU slots: one record per instance (RECORD_WORDS 10), updated in place
  (objects_update.comp), flags in word 7 (FLAG_SPINS 0x02000000 keeps a
  spinning part out of the far shadow and the sky occlusion).
- LOD: models from the generators carry per level errors (lod_meshes):
  a level is drawn while error * lod.x / distance < prop_lod_pixels (0.5).
  Other models go by height on screen (lod_base_pixels 300/120/50).
  objects.ini `lod_distance_scale` per type scales both.
- Spinning parts: objects.ini `spin = x,y,z,rpm`, `spin_pivot_meters`; the
  model's origin is the pivot (the sails' hub), the placement is offset by
  the rotated pivot; drawn as free objects in the near shadow's moving
  layer.
- Model formats: `.pobj` (object_format.h; pobj_pentru_fable.md at the
  root explains it), Eternal Lands `.e3d`. objects.ini names models
  as .e3d and the .pobj next to it wins.

## Shaders

GLSL with includes (common.glsl, constants.glsl, scene.glsl, shadows.glsl,
fog.glsl, lights.glsl...). Buildings: building.vert/frag with
prop_body.glsl (aging, moss, snow, sky occlusion lookup offset out from the
face). Plants: plant.vert (wind: gusts rolling downwind as waves) and
plant.frag. Each shader compiles to data/shaders/spirv/NAME.EXT.spv; an
include change rebuilds everything that includes it (check with grep -l).

## Decals and detail (step 34d)

- `decals.c/.h`, `data/decals.ini`, `data/shaders/decals.glsl`: decals are
  clustered with the lights in lights.c (`cluster_spheres` does both); the
  cluster buffer is now 4 words per cluster (lights first/count, decals
  first/count), then indices, then decal records (20 words), then the
  512x512 atlas (color layer, normal layer, RGBA8 as uints). The offsets in
  decals.glsl must match lights.c (DECAL_RECORDS_OFFSET, DECAL_ATLAS_OFFSET).
  Applied before lighting in prop_body.glsl (props, buildings), object.frag
  and terrain_body.glsl. Not in plants, grass, water, impostors.
- Projectiles leave `decal` from impacts.ini (not on OBJECT_STATE_FREE
  bodies, not on actors); explosions near the ground leave `scorch`.
- Roof variety (step 34f, prop_body.glsl, kind 3 = roof): per house
  (hash of its origin) lightness, fading, warmth; big weathered patches
  (grayer, lighter: what shows on dark slate), runs down the slope, mended
  stretches in the fresh color. ROOF_* defines at the top. A fine detail
  texture up close was tried in 34d and taken out: Radu wanted variety from
  afar, not grain up close.

## The shadows' draws in one call (35g)

objects.c `build_pool`: every model but the plants (32 bit indices: the
.e3d loader keeps 32 bit now) copied on the GPU into one vertex and one
index buffer (`pool_vertex_buffer`, `pool_index_buffer`), each model's
first index and vertex in the model table (floats 16, 17 as bits:
MODEL_FLOATS 20, the shader's MODEL_WORDS 5). The draw table puts the
draws that can go in one call first (`draw_batched_in_shadows`: a pooled
model's opaque, not deciduous material; DRAW_POOLED in its last word),
`shadow_batch_count` of them; in the shadow passes (far, near, the sky's
occlusion) objects_cull.comp writes their arguments for the pool and
`draw_models` draws them all with one SDL_DrawGPUIndexedPrimitivesIndirect
(draw_count = shadow_batch_count; draws of models out of view have no
instances). The rest (alpha tested, plants: uniforms of their own each)
go a draw at a time as before. On the test map 1488 of 1875 draws; the
gallery's shadows 1.2 ms of CPU a frame to 0.2; pictures and the sky's
occlusion the same as before to the pixel. About 70 MB more on the GPU
(copies). profile counter "shadow draw calls".

The camera's buildings too (35h): the buildings' models first in the
shared buffers, their tangents and weathering in two more
(`pool_tangent_buffer`, `pool_weathering_buffer`, as long as their part).
Their opaque materials' draws (building_material, DRAW_POOLED_CAMERA) come
first in the table, sorted by material (`compare_building_materials`:
the four textures, triplanar meters, thatch, shaded), then the rest of the
shadows' batch, then the others (`table_draw_t`, SDL_qsort_r). In the
camera's pass `draw_models` draws each run of one material with one call.
What was per model in the buildings' uniforms (the type's age, its spread,
lichen, moss) goes with each instance now: INSTANCE_FLOATS 15, the model
table's floats 20..23 (MODEL_FLOATS 24, MODEL_WORDS 6), copied in by the
scatter, building.vert's location 8 -> flat building_look (location 11)
in prop_body.glsl; show_parameters.y is the forced age (-1 none). The house
preview (tools/house_preview.c, INSTANCE_FLOATS 13) gives its instance the
same. Test map: the buildings' 1120 draws in 34 calls; the gallery's CPU
render time 3.1 ms to 1.5, pictures the same to the pixel. Memory: about
100 MB more on the GPU in all (the copies).

Not done: props (prop.frag's uniforms per draw), the others (object.vert's
wind per model), plants, the alpha tested (cut out details).

## Contact shadows (35s)

engine/contact_shadows.c, data/shaders/contact_shadows.frag. A full screen
pass after the sky, before the depth pyramid and the water's capture
(world_render.c, at the pause before the water): each pixel within 40 m
reconstructs its point and facing from the depth (the nearer neighbor of
each pair; an edge both ways skipped), looks up the shadow maps (nothing
to do where they shade it), marches 12 steps (t squared, jittered by pixel
and frame) up to render.contact_shadow_meters toward the sun; a hit is a
depth in front of the ray by more than a pixel and less than a thickness,
standing off the surface by more than 1.5 cm and a pixel. The sun's share
of the light there (sun by facing and map, against the sky and the ground
bounce) times contact_shadow_strength is taken away: GPU_BLEND_MULTIPLY
(new, gpu.c) over the scene color, alpha kept. Settings render.contact_shadows
(on), contact_shadow_meters (0.4), contact_shadow_strength (0.85), in the
Render tab under Shadows (settings_window.c and ui.c both).
Testing note: in the fast root the console's `set render.contact_shadows`
took effect only some frames later, and two screenshots differ by noise
(cloud shadows, shadow map updates) anyway: tint the pass's output in a
debug build instead of diffing screenshots.
35t: the occluder's assumed thickness 7 cm and two pixels (was 25 cm): a
thin stalk threw a strip of shadow behind it along the ray as seen from
the eye, different from every angle (the screen space's own failing: a
depth buffer has no back faces). A stone's still shades.
Soft (35t): no first hit cut off: each step's hit soft at each test's edge
(smoothsteps on the depth in front, the thickness, the standing), fading
along the ray as 1 - t^2 (what is farther off shades less), the max over
the steps; the ray's way turned within a cone of 0.06 rad round the sun's
(CONTACT_CONE) per pixel and frame: the anti-aliasing gathers it into a
penumbra (without TAA it is dithered).
Strength (35t): as dark as the maps' shadows at contact_shadow_strength 1
(the default; up to 1.5): the sun's share times shadow_strength taken away,
the hits' soft edges tighter, the fade only over the last 45% of the reach
(Radu: even at the maximum they were much fainter than the normal ones).

## TAA only while moving (35u)

render.taa_when_moving (on; "Only when moving" in the Render tab):
world_render() sets world->temporal_active only while the eye moved more
than a millimeter in a frame, and for 0.25 s after; standing or only
turning round, no jitter and no temporal pass (its history reset; back on,
it starts afresh). The objects' texture mip bias still follows render.taa
(no pop of the textures' sharpness as it switches). Radu: when standing
still it made more trouble than it solved.
35u: forward or back only (the eye's step along the view's middle, more
that way than sideways: Radu tested sideways is fine without it, and far
things flickered with it standing). Contact shadows' noise fixed per pixel
(no frame in it): no shimmer, with TAA off when standing.

## MSAA (35v)

render.msaa (1/2/4/8, default 4; at the next start: the pipelines are made
for it): gpu_set_scene_samples (potop_app.c, after gpu_create) picks what
the device can do for all three targets. render_target.c: the scene's
first part (terrain, objects, plants, impostors, actors, grass, sky) draws
into multisampled color + depth + an R32F "depth value" target
(gpu_pipeline_info_t.multisampled adds it as location 2, after the motion
target at 1: see the TAA below). SDL_GPU can neither sample nor resolve a
multisampled depth, so every shader drawn there writes gl_FragCoord.z into
location 2 (scene_depth.glsl: scene_outputs; the sky writes 1.0). The first render_target_pause
(before the water) resolves the color into color_texture and the depth
values (averaged at edges) into depth_value_texture, and depth_copy.frag
writes them into the plain depth_texture. Everything after (contact
shadows, hiz, water, clouds, fog, particles, rain, the weapon in hand)
is 1x as before.
Alpha to coverage (alpha_to_coverage on the object, prop, building, plant,
impostor, actor pipelines): alpha_cutout() in scene_depth.glsl, with
scene temporal_jitter.z 1, writes the alpha sharpened to a one pixel ramp
round the threshold as the alpha instead of discarding.
35x: grass.frag and plant.frag inputs centroid (with MSAA the thin blades'
values extrapolated off the blade: huge sky_light, bloomed blotches).
35w: impostor.frag divides by max(coverage, 1e-4) (NaN with A2C).
Radu: 4x cost ~40% fps and 8x still left aliasing: MSAA default off (1).

## Full-scene TAA (35y)

The old TAA jittered and averaged only the buildings (alpha 0.25 mark).
Now: world_render jitters the whole scene through scene.view_projection
(and its inverse; prop_vertex/object.vert no longer add it), Halton 16.
render_target (world_scene true: the world's only; the tools' previews
keep one color target) adds motion_texture (RG16F, location 1 of every
`multisampled` pipeline, cleared 0): how far the pixel moved by itself
since the frame before, in uv. grass.vert/plant.vert/object.vert compute
their wind offset twice, at wind.w and wind.w - temporal_jitter.w (the
wind clock's step since the last frame: world->temporal_wind_step), and
scene_motion() (scene.glsl) gives the screen delta; the other shaders write
0 (actors: their own motion unknown, the box clamp holds them).
taa.frag: every pixel; the nearest depth's pixel in 3x3 unjittered by
temporal_parameters.zw, reprojected by the camera, less the motion;
Catmull-Rom history, variance clipped box in YCoCg; less history the faster
it moves. post.frag sharpens after (render.taa_sharpen, 0.3; clamped to the
4-neighbors' range: no halos), only when the TAA ran. The texture mip bias
(taa_mip_bias) now on all the objects' camera pass, not only buildings.
Defaults: taa true, taa_when_moving false, msaa 1. The alpha marks are gone
(prop_body, object.frag write the cut out coverage, 1 elsewhere).

## TAA back to buildings, from a distance (35z)

Radu: the full-scene TAA turned the vegetation into a VHS haze (leaves
under a pixel melted; worse in the wind). Now: no jitter in the scene's
matrix; taa_shift() (scene.glsl) jitters only the buildings
(prop_vertex.glsl BUILDING, object.vert building details), scaled by
taa_share(clip.w): from render.taa_start_meters (20) to 5 m further
(scene.gusts[2] carries it). The motion target's b marks the averaged
pixels (buildings 1, all else 0); taa.frag returns the raw color where
b * the share by distance (view_distance from depth, near/far in
distance_parameters) is 0, and writes that amount in alpha; post.frag
sharpens only by it. The mip bias is buildings only again. The wind motion
vectors (rg) stay, unused by the buildings for now.

## Motion target removed (36c)

Radu: frame rate fell even with TAA/MSAA/contact shadows off (sky 350,
grass 180 fps). The 35y motion infrastructure ran always: an RGBA16F
target written over the whole screen and every grass/plant/object vertex
evaluating its wind twice. Gone: no motion target, no scene_motion, the
wind evaluated once (blade_point / wind_offset kept as functions). The
buildings' TAA mark is the alpha 0.25 again (scene_alpha: lost with MSAA,
where the alpha is coverage); taa.frag reads it, gated by distance as 35z.
scene_depth.glsl: location 1 the MSAA depth value only, scene_depth_write.

## TAA back to 35u (36e)

Radu: the reworked TAA (distance gate, sharpen, unjitter) did nothing for
him and the house still shook as he moved, the same with TAA on or off: the
shake is not the TAA. taa.frag, post.frag, post.c/h are 35u's again
(buildings only, alpha 0.25 mark, camera reprojection); the jitter in
prop_vertex/object.vert as before; taa_start_meters and taa_sharpen gone;
defaults as 35u (taa on, only going forward on). Kept: MSAA (default off),
centroid, the impostor NaN guard, the motion target removal.

## Window lights (36f)

lights.c gather_windows: each lit emissive group named "window"
(objects_light_windows decides which) of the objects within 90 m gives a
light: window_light_color (warm), render.window_light_intensity (0.4),
6 m radius, 0.5 m out from the glass's center and 0.3 m up. The way out
(36p): the record's flags, OBJECT_EMISSIVE_FACING (2u) with the yaw in
the second byte (OBJECT_EMISSIVE_FACING_SHIFT 8, 0..255 a whole turn from
+z towards +x); house_grow.c record_facing writes it, from face->out in
glass() and from the quad's normal in window_group_at (workshop glass).
Without the flag (older files) window_outward falls back to the model
bounds' side nearest the window's center, which put the light behind
wings, porches and yards (a pool on open grass: Radu's 36o bug).
gpu_light_t is now three vec4 (lights.glsl light_data stride 3): side.xyz
the facing, side.w > 0 one sided: lit only in front of the plane side.w
behind the light (the glass), smoothstep over 20 cm, so the wall right
round the window takes a grazing light and nothing goes through to the
inside. lights_light_at honours it. scan_windows runs with scan_objects
(every 0.5 s or when the instance count changes). Settings:
render.window_lights, window_light_intensity (Sky & light tab).

## Building spots and the hens (36h)

.pobj version 12, OBJECT_FILE_SPOTS (object_file_spot_t after the
footprints, at most 32): name, model-space position, yaw, area extents
(half x, z; 0 = point), 4 links (indices, both ways: straight clear ways,
baked by the generator). object_model loads them; objects_spot_count /
objects_spot_world (world position, yaw + instance yaw, scaled extents).
potop.h: potop_object_spot_count / _get / _find (potop_spot_t),
potop_actor_walk_straight (actors_walk_straight: actor_t.straight, the
given points walked with no search, no look-ahead, no replans; each step
checked with navigation_passable and slid along x or z, else stopped; stuck
3 s -> stop + NO_PATH), potop_actor_remove (actors_remove).
game/hens.c: hen houses (type name hen_house*, with a "yard" spot) within
120 m of the walker; 7 hens each come out by day (DAWN 0.35 daylight at the
house) one by one at a "hen_ramp_foot"; roam: random point in the yard
area, 35% to a "peck"/"drink" spot (a few pecks: action "eat"), 40% a
ground peck after a walk; at dusk (0.25) each walks foot -> hole straight
and is removed there (or stopped within 1.5 m of it: the nav grid may not
let it through the 26 cm hole). Taken in too past 160 m. places.c: only
named actors announce entering/leaving areas now (the hens spammed it).
actors.ini [hen]: data/actors/hen.pact from Fable's glTF (walk, eat, idle).
36i: straight walkers are blocked only when the step leaves a passable
point for an impassable one (a hen the nav grid wrongly puts inside a
feeder's cell trod in place forever), and a full stop zeroes the speed (no
walking on the spot). keep_apart (actors_move): ground actors closer than
their body radii (0.4 x the larger of width and length) are pushed apart,
half the overlap each, O(n^2).
36j: straight walkers no longer use the navigation grid at all
(NAVIGATION_CELL_METERS is 1 m: a feeder or a fence took whole cells, the
hens stopped short of the feeders and trod in place). straight_blocked:
two physics_cast_segment lines ahead (feet at min(8 cm, 0.3 h), middle at
0.6 h; length step + 0.4 width + 5 cm), a body hit (not the ground) ends
the walk: ARRIVED if within 0.8 m of its end, else NO_PATH; no sliding
(it made them walk sideways). actors_t.physics set by world_create.
hens.c: a walk to a feeder ends turned to it (look_at) with 1-3 pecks.
36k: hens caught in a feeder's trough: keep_apart pushed them into it,
and from inside a mesh every way out is a wall. Now keep_apart does not
push one into a body (straight_blocked on the push; the other takes it
all); straight_blocked reaches out to the beak (1.25 x body_radius + 3 cm
past the step); and three walks in a row stopped where they set out from
(actor_t.straight_blocks) give the next 0.6 m free of collisions
(straight_free_meters), to get out of whatever caught it.
36l: hens.c going in: walk_in (nearest hole, its ramp's foot, the hole);
stopped short by something (not within 1.5 m of the hole): a detour to a
random point in the yard, then walk_in again from there, as often as it
takes; in anyway after 90 s.
36m: a straight walker also stops (ARRIVED near its end, else NO_PATH)
when another ground actor stands ahead of it (straight_actor_ahead: along
its way within their body radii plus the step, not off to the side past
them): it goes elsewhere instead of shoving the other, which slid where it
stood. Such stops do not count toward straight_blocks (only bodies do).

## Farm animals, one mind (36n)

game/hens.c became game/animals.c: one mind, a kind table (animal_kind_t:
actor type, home type prefix, how many, feeders and their share, ground
eating, a stretch action, the way in as a chain of spot names, vanish at
the end (hens) or a sleep spot (cows: "byre_stall", half of them inside),
lie_down / sleep_idle / get_up). States ROAM, BUSY, GOING_IN, GOING_DOWN,
ASLEEP, GETTING_UP, GOING_OUT. Hens: off the map at night, out one by one
at dawn. Cows: stay; at dusk half walk byre_door_out -> byre_door_in ->
a free stall and lie down there, the rest lie down in the yard; at dawn
get up (the inside ones walk the way back out). Ground: a home's "yard"
area, or (game_add_free_animal) a circle round a point for loose ones,
which sleep where they stand. cow_farm: the right byre door now open;
spots "yard", "hay" x4 round the rack, "trough" x2, "byre_door_out/in",
"byre_stall" x6. actors.ini [cow]: scale 1.5 (the model is 0.93 m tall).
36o: a killed farm animal (actor.dead) stays as a corpse (ANIMAL_DEAD) for
300 s, its place at home still taken; then it is taken off the map and
its home brings another out 180 s later (it used to be replaced at once,
the corpse left lying for good).

## Turning (36r)

actor_t.turn_rate: radians a second it turns (> 0 to its left: from +Z
towards +X), worked out at the end of move_actor from the yaw's change and
eased (8 a second). choose_animation: standing and turning faster than
TURN_ON_SPOT_RADIANS (0.2), it plays turn_left / turn_right if the model
has them (actors.ini turn_left_action / turn_right_action, defaults those
names, optional), at turn_rate over turn_degrees_per_second; else the walk
at the pace of its feet round its middle (turn_rate * body_radius over the
walk's pace, 0.3..1.5). walking_action() (walk, idle, the turns): the only
actions choose_animation replaces, besides the loops it left as before.
Arcs: turn_walk_fraction (actors.ini, default 0) for straight walkers: in
a sharp turn it keeps that much of its walk (limited so the goal stays out
of its turning circle: distance * turn speed / 2) unless the first step of
the arc is blocked (straight_blocked / straight_actor_ahead): then on the
spot as before. 36s: Fable's turn_left / turn_right clips (in place, the
body not turned in them, looping): cow 0.8 s made for 45 deg/s, ram
0.67 s for 60 deg/s, so turn_degrees_per_second 45 and 60 play them at
rate 1. tools/convert_gltf.c guess_loop takes "turn" as a loop; a skin
over ACTOR_FILE_MAX_BONES (64) joints loses the joints that move no vertex
and have none under them that does (needed_joints: Blender's "_end"
leaves; the ram: 71 joints, 54 bones), bone_of_joint 0 for them.

## Sheep at the stana, livelier cows (36t)

animals.c kind "ram" (actor type ram, home prefix "stana", 12 a home): a
flock. animal_kind_t.flock_radius (5 m): point_on_ground picks round the
home's grazing place (animal_home_t.graze_x/z, a random point of the
"yard" area, moved every flock_move_least..most = 25..70 s), so the flock
drifts over the pasture together. sleep_area ("fold"): at dusk each picks
its own point in that area spot (animal_mind_t.has_sleep_point,
sleep_x/z), the end of its way in (way_in); chain fold_approach,
fold_gate_out, fold_gate_in (through the strunga's narrow way); inside
share 1. Brought out at night: spawned asleep in the fold, out by the
strunga in the morning. A flock's detour (blocked going in, a crowd at the
gate): point_near, 1.5..3.5 m round where it stands, then the way again.
point_in_area: the old yard code, for any area spot. Bites in a row:
feeder_bites_most / ground_bites_most (hen 3/1, cow 2/1, ram -/4).
Cows livelier (Radu: too much idle and eating): feeder_share 0.4 -> 0.25,
ground_eat_share 0.45 -> 0.2, stretch_share 0.2 -> 0.08, stand 2..8 s ->
0.8..3 s, at most 2 bites at a feeder.

## Steering, the trace, the first fixes it found (36u)

actors.c straight walkers steer now (Radu's "on collisions, not path
finding"): steer() tries the heading to its waypoint, then the nearest
free one round it (25 degrees apart, out to 125 each side), keeping the
side it went round on for 1.5 s (no swinging). heading_free: no body
(straight_blocked) and no other just ahead (straight_actor_ahead) along it
for 0.3 m plus its body's reach. Nothing free: others in the way, it waits
(crowd_seconds) up to 3 s, then NO_PATH; bodies all round (or near its
end) ends the walk at once (ARRIVED within 0.8 m). Caught (bodies all round
where it set out from three walks in a row): the next walk goes 0.6 m
free. The step itself no longer ends a walk: blocked the way it faces
(still turning to the free heading), it holds still.
Found with the trace (00_START_HERE, "Testing the animals' minds"):
- cows never got into the byre: its collision was one solid box over it
  all (now walls, the right door open) and the open door's dark quad
  (HOUSE_MATERIAL_GLASS) was solid across the hole: glass is
  OBJECT_MATERIAL_PASSABLE now (only regenerated houses have it: cow_farm,
  hen_house so far);
- a cow asleep in the yard stopped the others going in for good (detour,
  way in again, the same sleeper in the way): steering goes round;
- the hens' peck and drink spots stood inside the feeders' feet: 0.6 and
  0.75 m out now;
- sheep jammed in the strunga at dawn and roamed inside the fold when
  their way out failed: ANIMAL_GOING_OUT retries from the nearest point of
  the way out (way_out_again) up to 90 s; animal_kind_t.dusk_spread and
  dawn_spread (cow 30/40 s, ram 90/120 s) send them in and out one by one.
Still seen: now and then a hen walks into the water trough (the escape
gets it out in about 40 s); sheep mill a while at the strunga at dawn.

## Pigs at the pigsty (37c)

animals.c kind "pig" (actor pig, home prefix "pigsty", 4 a home): two
pens. animal_mind_t.pen: the home's pens in turn at bring_out (pens = how
many "yard" spots it has); nth_spot gives the pen's "yard" and sleep area
("sty"); feeders, with pens, the nearest of the name to the pen's yard.
Chain pig_door_out, pig_door_in (each pen's own door: the nearest).
Sleep area along the back wall (one lying behind the door kept the other
out all night); in_near 1.7 m (stopped that near its place, the other pig
in the way: in); with a sleep area a detour going in is point_near (it
was off across the yard from inside the shed). inside_area(): stopped in
its sleep area counts as in. actors.c: STRAIGHT_WAYPOINT_METERS 0.25 (a
straight walker's waypoint is its mind's spot, a door's before it: 0.6
cut the corner into the jamb). Pigsty (house_grow.c workshop 11): the
front wall in pieces round the pigs' doors (hut_opening only painted the
dark on the wall: no hole), its collision hollow (walls, the doors free;
pieces[] 16 now), spots yard/trough/pig_door_out/pig_door_in/sty a pen.
actors.ini [pig] given to Radu (scale 1.2, walk 0.7, cycle 0.45, turn 60,
turn_walk 0.4). Trace: all four in, about 30 s.

## Backing out (37d)

actors.c, straight walkers: its way more than BACK_TURN_RADIANS (50
degrees) round and a body within BACK_NOSE_METERS (0.1) past its reach
ahead (its nose in a trough, against a fence), nothing behind it (no body
within 0.3, no other: straight_actor_ahead backwards): backing_meters =
BACK_MOST_METERS (1.0). Backing: heading kept (no turning), wanted speed
-BACK_SPEED_SHARE (0.35) of its walk, the walk played backwards
(action_rate < 0; loops wrap below 0 now), no arc; over when its front is
clear by BACK_CLEAR_METERS (0.35 past its reach), something is behind it
(body or other, 0.2), or the meter is used. Then it turns as before (the
pivot, or the arc once the first step is free). Radu: "should be core, for
every animal". In the trace: cows all in at 25 s (were half late), pigs
17 s. POTOP_DEBUG_ACTOR=<id> logs that actor's every frame (place, yaw,
heading, speed, backing, crowd).
Seen, not fixed: hens walk into the water trough: its top is within their
step_meters (0.3 in Radu's ini), so the body is lifted onto it and their
casts go over its sides (the samples under the body take object tops
within the step). step_meters 0.1 for small animals would stop it.

## Big bodies turning, the hay rack (37e)

Cows got tangled in the hay rack. Two causes:
- the rack (farm_hay_rack: legs only at its ends, rails and slats from
  0.45 to 1.25 m) was invisible to a cow's two lines (feet 0.08, middle
  0.6 of its height = 1.26): it walked in. cow_farm collision now has a box
  for it; and two of the four "hay" spots stood at its ends, inside its
  3.6 m length: now two a long side.
- turning on the spot swings a 3 m body's rump through what the line
  ahead never saw. body_fits(x, z, yaw): lines from the middle to the
  four corners (bounds 0.85 across, 0.9 along, scaled), feet and middle.
  Only for big_body (half its length past BIG_BODY_HALF_METERS 0.9: a cow;
  sheep and pigs in a strunga or a sty were only kept from turning, the
  trace got much worse). A turn the body does not fit is not made: it
  creeps ahead (0.35 of its walk) if its front is free, else backs out
  (half a BACK_MOST_METERS); standing (facing), it just stays. Not for
  steps (it stopped them in every doorway). In --sim, log "its body
  caught in something" each time a big one's body stops fitting (its
  nose over the rack while eating counts).

## Modules (.pmod, 37f)

A module is a building made of several objects that stand and move
together, with states. Binary only (engine/module_format.h), written by
tools (potop_house now, an editor later), never by hand. Shared reader and
writer: engine/module_file.c (plain C, linked by the tools too);
`tools/potop_module FILE.pmod` dumps one.

- File: header, parts (name, model under objects/ without extension,
  position in the module frame, yaw, axis, spin_rpm, flags COLLISION /
  SHADOW / MOVES / IMPOSTOR), states (name, seconds to get there,
  hold_seconds + next: goes on by itself), poses per state per part
  (offset, angle round the axis, spin share, HIDDEN).
- Part 0 is the body: the module's origin, never moved. objects.ini keeps
  one short entry: `[windmill] module = windmill` (+ category, footprint).
  object_types.c (resolve_module) reads data/modules/NAME.pmod, gives the
  type part 0's model and its physics/shadow/impostor from the flags, and
  appends one type per other part, named `TYPE.PART` (module_type,
  module_part set; MOVES parts: no body, no impostor). The editor lists and
  the --houses gallery skip part types.
- Placing the module type (objects_add, so layouts, the editor, the
  gallery, map loading all work) places its parts after it, then swaps the
  body back to the end of the list (callers take instance_count - 1).
  Parts: OBJECT_STATE_PART (+ MOVING, HIDDEN), module_id = the body's id,
  module_part. The body keeps module_state / module_target /
  module_progress / module_held_seconds; a part keeps module_turns (its
  spin so far).
- object_modules.c: objects_update_modules (world_begin_frame, every frame)
  advances states, re-places moving parts (x, z, lift_meters, rotation,
  HIDDEN) and parts whose ground is no longer the body's; pack_slot treats
  MOVING like spins (off the grid, FLAG_SPINS). Removal of any piece widens
  to the whole module (compact). objects_set_id renumbers the parts.
  Maps keep the parts' records but loading skips them (the body puts them
  back); the module's state is not saved yet (placed in first_state).
- Console: `module [state]`: the nearest module (40 m) and its states, or
  sends it to one. API: objects_set_module_state, objects_module_state,
  objects_nearest_module, objects_module_of.
- Not yet: rope stretching (the quarry's rope is 4 lengths sliding over
  one another instead), parts parented to parts, saving the state,
  moving a module in the editor moves only its body, the building
  scripts and the workers that drive them.

## Workers (37j: the smith)

- 37w, his life: one smith per smithy on the whole map (scan every 5 s
  within 5 km, potop_objects_with_spot "smith_anvil"), never taken off
  while his smithy stands. Home: the nearest free object with a `door`
  spot within 400 m (not a smithy; none: he sleeps at smith_rest); inn:
  the nearest `inn_door` within 800 m. His day (life_day_t, drawn again
  each day, the day ticks when the hour wraps): work from ~7, lunch ~12-13
  (40% at the inn), work till ~18, the inn till ~21 (25% of evenings
  home), home. life: LIFE_WORK (the round as before), LIFE_TRAVEL,
  LIFE_INSIDE (no actor). The work ends at the next pause: leaving 1, then
  wind_up (the piece on the rack), then straight off (38b: no yard point first). An actor only
  within 480 m of the walker (taken away past 520 m): with one he
  potop_actor_go_to's the place (a door: within 4 m, the rest straight
  onto the spot facing it, then removed; stopped short: 3 more tries, then
  put there); with none he walks his route (potop_find_path, kept in
  worker_mind_t.route, 128 points) at 1.3 m/s. Out of a door: 3 m
  straight out first (porches, steps). POTOP_DEBUG_WORKERS=1 logs his day.
- 37x: to a door by a point 1.2 m straight before it, then onto the spot
  (he cut the stone step from the side). The step stands 0.3-0.7 m over the
  ground (one slab under the floor's level): too high a stride, not a
  navigation surface; real stairs in the generator would let him climb it.
- 37y: people (ground_align <= 0) stand on what the physics has under
  them too (place_on_ground: physics_walker_ground, up to step_meters over
  the feet, the higher of it and the navigation's surface): a door's step,
  a terrace, by the objects' real shapes. Only the height: what is higher
  than a stride still is the navigation's to walk round (no sliding along
  bodies on a found path yet).
- 37z: doors on high floors have stairs (house generator door_way): the
  door's spot at their foot, "<name>_sill" (door_sill, inn_door_sill) at
  the top before the door. The smith goes to the foot (go_to), then
  straight: a point 1.2 m before the foot, the foot, the sill (up the
  stairs: physics ground), then in; he comes out at the sill, down to the
  foot and 1 m on, then on his way (worker_mind_t.exit_count: those first
  route points). straight_blocked: people look for bodies over a stride
  (step_meters + 3 cm), not at 8 cm (a stair stopped him).
- 38: his dice seeded with the clock too: the smithy's id (kept in a map's
  file) alone gave him the same day at every start (always home).
- 38a: to a door his actor is sent to a point 1.2 m before its spot
  (travel_point), and once within 2.6 m and before it (a cone of ~57
  degrees out from the door: before_door) he goes straight on up to it and
  up its stairs without stopping (door_walk); from beside it, by that
  point first. (He went to the stairs' foot, then out 1.2 m and turned
  back.)
- 38b: the work done, straight off: no yard point; from inside the smithy
  out by its nearer smith_door_ spot first (walk_straight, from_door), then
  potop_actor_go_to (no way is found from among the anvil and the trough).
- 38c: due off, the rest of his plan dropped at the end of the step he is
  on (it waited for the whole queue: a break's idling ran minutes), and
  idling (any step not smith_) cut short after 2 s.
- Engine (37w): potop_find_path(potop, actor_type, from, to, points, max,
  complete) (navigation_find_path over and over, no actor);
  potop_objects_with_spot(potop, x, z, radius, spot_name, ids, max).
- Test: `xvfb-run -a ./potop --houses smithy,cottage,inn,hovel --width 320
  --height 240 --set render.day_cycle=true --set
  render.day_length_minutes=24 --hour 6.5 --sim 900` with
  POTOP_DEBUG_WORKERS=1 (a day in 15 minutes, as fast as it goes).
- game/workers.c: a smith ("blacksmith" actor type) spawned in each object
  near the eye (120 m; off past 180 m) that has a `smith_anvil` spot. His
  round (SMITH_ROUND): forge 5 s, bellows 6 s (the building's module to
  `pump_down`, which goes on by itself with `pump_up`; `still` after),
  forge, hammer 7.2 s, forge, hammer, quench 2 s, a breath at the anvil.
  Phases: walking (potop_actor_walk_straight_exact to the spot), facing
  (potop_actor_look_at the spot's way, till 2 degrees or 1.5 s), working
  (his clip, the step's seconds). POTOP_DEBUG_WORKERS=1 logs each arrival
  (how far off, how many degrees).
- Engine: actors_walk_straight_exact (actor_t.arrive_meters 0.04): slows,
  turns on the spot when the goal is not ahead, the last 16 cm shuffled
  onto it, never backs out (backing is for big animals). potop.h:
  potop_actor_walk_straight_exact, potop_object_set_state (a module's
  state by name).
- The bellows clip and the module run on the same 2 s: pulled down by
  0.75 s, held, let up by 1.75 s (pump_down / pump_up: 0.75 s each, held
  0.25 s), started together.
- Not yet: the hammer and tongs in his hands (no attaching to bones yet),
  the fire brighter while the bellows blow, his day (home, tavern).
- 37k: navigation_surface_height takes an object's top only within the
  agent's step_meters of where it stood (the slope's reach, up to
  tan(slope) x 1 m cell = 0.84 m, is the terrain's): a smith by the anvil
  stood on its horn, hens in troughs. Animal test (1200 s, 16:00), before
  -> after: cows walking in place 80 -> 40, rams 27 -> 9, pigs in for the
  night 54 -> 16 s median, rams 46 -> 34 s; hens inside an obstacle
  5.1% -> 5.6%. People (ground_align 0) stand on the point under their
  origin. Exact walks keep their pace to the last 16 cm (no slow-motion
  approach), then shuffle.
- smithy_collision now uses smithy_works' places (it had the old smithy's
  layout: boxes where nothing stood).
- 37l: actors_walk_straight_exact(..., face_yaw): turned to face_yaw the
  moment it arrives (no idle between); a goal under SIDESTEP_METERS 0.7:
  no walk, turned on the spot to face_yaw at its turn speed, sliding onto
  the point meanwhile (at least 0.6 s; no turn to make: the turn's steps
  all the same), snapped there, ARRIVED. The turn clip starts at 0.5 rad/s
  (TURN_START_RADIANS), goes on down to 0.2; turn_rate settles 20/s,
  starts 8/s (no steps in place after a turn). Workers come from farther
  than 1.5 m by a point 0.6 m before their place (so they walk in facing
  their work). Smithy hood mouth 2.05 m (was 1.80: his head went through).
- 37n, people's steps (actors.c "Steps between standing and walking"):
  clips found by name (ACTOR_GAIT_: walk_start, walk_start_180,
  walk_stop_l/_r, step_left/_right/_back, shuffle); how far each moves
  from actors.ini (walk_start_meters 0.366, walk_start_180_meters 0.4,
  walk_start_180_delay 0.33, walk_stop_meters 0.327, walk_stop_left_phase
  0, walk_stop_right_phase 0.5, sidestep_meters_per_second 0.833,
  step_back_meters_per_second 0.625: the blacksmith's, from Fable's
  docs/claude/assets/blacksmith_travel.json). A gait under way moves the
  actor itself (eased: a start gathers pace, a stop loses it), no
  steering, no crossfade in or out (start_action_now). Off from standing
  on a walk long enough: facing within 0.8 rad, walk_start; the way behind
  (> 2.4 rad), walk_start_180 (its clip turns him; yaw += pi at its end);
  between, turned on the spot first (start_held). Nearing an exact goal
  (3 m), the walk's stride is stretched 0.8..1.25 (stride_scale: the walk
  clip's rate divided by it) so a foot lands one stop away; the stop on
  that foot begins at its landing (whichever leaves the goal nearer the
  stop's length). Sidesteps with < 0.5 rad to turn use step_left/right/
  back, the walk at 0.6, or shuffle (< 12 cm); the exact walk's last
  shuffle plays shuffle. POTOP_DEBUG_GAIT logs the stop's planning. The
  workers' lead-in point is off (its corner broke the stop).
- 37o: potop_actor_info_t.moving is true while an actor steps onto its
  place or is in a gait (a worker's clip no longer cut by the end of his
  sidestep). Smith's bellows spot 0.3 m beyond the handle (was 0.4); the
  bellows' moving leather reaches 1.4 x the rise + 6 cm down inside (a gap
  showed fully open).
- 37p, the smith's work by state (game/workers.c rewritten): the smithy's
  workshop_t (fire 0..1 dies 0.012/s, bellows +0.14/s, coal +0.25; iron
  heat in the fire +0.11/s x fire, out of it -0.07/s; a piece wants 14-30 s
  of hammering). After each plan he picks: no piece -> hang the last up at
  smith_rack (60%: a breather out front in smith_yard, a look round / arms
  crossed / hands on hips / stretch / wipe), take the next, into the fire;
  fire < 0.4 -> coal (sometimes) + bellows till it is up; piece done ->
  (inspect) quench; hot (> 0.85) -> anvil, heavy then light blows as long
  as the heat holds (sometimes an inspect between); else -> the forge while
  it heats (a shovel of coal, a wipe or a glance round meanwhile). Steps:
  a queue (worker_step_t: a spot, a point or here; the clip, seconds,
  bellows pumping). To and from the rack by smith_aisle. New spots:
  smith_rack, smith_yard (area), smith_aisle. Bellows spot 0.28 m.
- 37q, the smith walked a third of his time (tools: POTOP_DEBUG_WORKERS=1
  logs each step with the sim time; a script that sums walking and clips
  from it was used, see the 37q notes): slower heat (0.05/s in, 0.025/s
  out: hammering in 20 s bursts), pieces of 30-70 s, the fire dying at
  0.006/s and blown up while the iron heats only under 0.5; the bellows'
  lever turned to the front (its handle by the hearth's corner, the smith
  a step from the fire, was 3 m round the back); the anvil spot beside the
  anvil facing along the hearth (a quarter turn from the fire, was a
  half); while the iron heats: coal, a wipe, a glance, a few strokes of
  the bellows, now and then his mug at the bench. 30 min sim: walking and
  turning 33% -> 19-23%, moves 10 -> 7 a minute, places 5.5 -> 4 a minute,
  one stuck (a turn-about start backs him without looking).
- 37r, the smith's breaks: out front for real (smith_yard: the whole width
  of the smithy, 1-4.6 m out; a point the grid refuses ten times taken
  anyway), 1-2 stops after 45% of pieces, and a proper break of 3-5 stops
  (several minutes) once he has worked 3-6 minutes (workshop_t
  worked_seconds / break_after); at each stop pastimes: arms crossed
  8-25 s, hands on hips 8-20 s, a look round, his mug twice, a stretch, a
  wipe. 30 min sim: idling 22% of his time (was 3%), 17 stops out front
  (was 5, each a couple of seconds).
- 37s: a heat planned in one go (plan_heat): the iron in, the bellows if
  the fire is under 0.5 (at least 8 s, till it is up), then the whole wait
  at the fire (at least 10 s; coal, a wipe or a glance in place), then the
  whole heat at the anvil (at least 12 s; near enough hot counts: no trip
  for 3 s more). No more 2-5 s visits back and forth. 30 min sim: a place
  held 15-30 s at work, 3.1 changes a minute.
- 37t: breaks show sooner (they came only after the first piece, 3-4
  minutes in, so nobody watching ever saw one): pieces 20-45 s, a breather
  after 60% of them, the first proper break after 50-100 s of work (then
  every 2-4 minutes), and half the smiths start with a breather when they
  appear.
- 37u: a heat topped up, not begun again (2 s least once the iron is in;
  near enough hot = 0.2 short); the forge wait shared out exactly round a
  pastime. Actors: a goal too near and too far round for the turning circle
  at its pace (2.2 x circle + 0.2 m, off > 1 rad) turns it on the spot
  first (a smith went round and round a point out front: the old guard was
  off for people); the turn on the spot before a walk's start gives up
  after 2.5 s; a walk's start only on walks 0.9 m longer than start + stop.
  Smithy: smith_door_left / _right (in and out from the front: left of the
  anvil's stump, right of the trough for the rack: the trough barred the
  aisle from outside); out front, the next place at least 2 m off. Sims of
  20 min: 0-1 stuck (was 2-4).
- 37v: the bellows rope its own module part (smithy.bellows_rope), only slid
  up as the board opens; the pole turned just enough that its short end
  keeps pace with the board (it used to swing the rope off the bellows).

## Physics bodies on a big map (38d)

- sync_bodies' 2 ms budget counted from the start of its pass over every
  object: on an 8 km map going through them alone took longer, nothing far
  was ever made, needs_full_sync stayed set and every step went through
  millions of objects again (21 ms: a stutter every physics step). Now the
  budget is the time spent making bodies (making_ticks), and what is left
  goes on in continue_bodies: from physics_t.bodies_cursor, at most
  BODIES_LOOKED_AT_PER_STEP (200 000) objects looked at and 2 ms of making
  a step (bodies_pending), no full pass; a change of the object list still
  means a full sync.
- 38h: bodies only round the focus: made within BODIES_RANGE_METERS
  (1000 m), taken away past BODIES_DROP_METERS (1100 m); when the focus
  has gone 64 m from where they were last gone through, continue_bodies
  goes through the objects again a slice a step, making and taking away
  (physics_t.range_focus_x/z). With every object of an 8 km map a body
  (38d let them all be made) the step's loops over the bodies (the water's
  push: 4-12 ms) and Box3D grew. Farther than that nothing is hit (a
  bullet flies through) and nothing floats or is uprooted.

## Drawing from the eye (38e)

- camera_t.view_projection_relative / inverse_view_projection_relative:
  the projection times a look_at from the origin (no translation); scene
  uniforms carry view_projection_relative (appended to scene_block, the
  size assert 5 * 64 + 16 * 22). scene.glsl world_to_clip(world) =
  view_projection_relative * (world - eye_position): every gl_Position
  from a world position goes by it (actor, grass, impostor, light_core,
  object, particles, plant, prop_shell / prop_vertex, rain, rain_splash,
  snow, terrain, water), and the clip positions of lights.glsl,
  water_ssr.glsl, contact_shadows.frag, impostor.frag, rainbow.frag.
  Reason: a float32 world position kilometers out times a matrix carrying
  the eye's translation lost millimeters that changed with the eye frame
  to frame: the houses trembled under the TAA on a 4 km map.
- The TAA's reprojection: previous relative VP x translation by how far
  the eye moved x this frame's inverse relative VP (world_t.temporal_eye,
  temporal_previous_eye).
- Left absolute (millimeters do not show there): the shadow maps'
  matrices, the reconstructions from depth (inverse_view_projection: fog,
  clouds, sky), the culling (objects_cull, grass compute, hiz).
- 38f: the rays from the eye too: scene_block's
  inverse_view_projection_relative (size assert 6 * 64 + 16 * 22) and
  scene.glsl clip_to_eye(ndc, ndc_depth) (world minus eye_position): the
  clouds' composite, fog_apply, fog_simple, fog_inject, fog_rays_mask,
  water_reflection, contact_shadows, sky.vert, rainbow.vert. The inverse
  with the eye kilometers out and the far plane kilometers off gave rays
  that wobbled frame to frame (the volumetric clouds flickered as the
  houses had).

## Objects near a point (38g)

- potop_objects_near and potop_objects_with_spot go by the objects' grid
  (objects_in_circle in potop.c: the 32 m cells round the circle, then the
  loose objects after the last cell; a pass over all of them only while
  grid_dirty). They went through every object: on an 8 km map (millions)
  tens of ms a call, and Moș Martin's chest is looked for every frame
  while there is none ("game 30 ms" stutters). The order they come in is
  now the grid's (the animal test's numbers moved by chance: the homes
  are found in another order). potop_objects_with_spot marks the models
  with the spot first (no name compared per object). The smiths' scan of
  the map: every 30 s (was 5).

## No passes over every object each frame (38i)

What went through every object (or every physics body) often, now:
- objects_update_spin (every frame with a spinning type): a list of the
  spinning instances (objects_t.spin_instances), found again on a new
  generation.
- physics_step, the water on the bodies: lists (physics_body_t.list_bucket,
  list_previous, list_next; body_list_insert / _remove in create_body,
  destroy_body, uproot): the loose bodies' list, and per ground tile the
  anchored or breakable ones; a step looks at the loose ones and the tiles
  whose water tile is active (gathered into physics_t.visit_slots). Static
  unbreakable bodies are in no list. damage cooldown and the rescues'
  window by physics_t.clock_seconds (damage_ready_seconds,
  rescue_until_seconds), not counted down on every body every step.
- recover_flattened: physics_t.flattened_ids (remember_flattened).
- wake_bodies_in_rectangle: the objects' grid cells over the rectangle
  (fixed bodies) and the loose list.
- objects_visit_circle(objects, x, z, radius, visit, user) (objects.c): the
  grid's cells round a circle and the loose objects; potop_objects_near,
  potop_objects_with_spot, object_effects_update (within effects_meters),
  lights' scan (within LIGHTS_FAR_METERS + 60 m) go by it.
- objects_light_windows: a pass of OBJECTS_WINDOWS_PER_FRAME (100 000)
  objects a frame after each new minute (windows_cursor).
- ground_footprints: the objects that may have a footprint
  (ground_footprints_t.standing), found again on a new generation; the
  signature and the marking go through them.
- harvest_update: objects_t.regrow_ids (picked out, waiting to grow
  again), found among all once when not known (a map loaded,
  objects_clear).
- ground_items_instance: ground_item_t.object_id (by objects_find_id);
  a loaded map's found by group once.
Still whole passes, but only when the object list changes: the sort, the
physics' full sync, flush_slots, refresh_models_used.

## Strays and the dead: no sort in the game (38j)

A barrel the water took (objects_set_free) used to dirty the grid: the
whole list sorted again (a counting sort over cells x types: ~80 MB on an
8 km map, 44 ms), a new generation, and everything kept by index went
through it all (the physics' full sync 50-70 ms, the id table, the slots,
the navigation's every region). Now:
- objects_add_lifted puts it at the end (from sorted_count on), no sort
  (a module still sorts); objects_set_free leaves it where it is, a stray
  (objects_t.strays); objects_retype turns one into another type in place
  (a stray: out of its cell's order). objects_sort sorts again only when
  grid_dirty (something taken out of the list) or more than
  OBJECTS_ADDED_BEFORE_SORT (4096) added / OBJECTS_STRAYS_BEFORE_SORT
  strays.
- Gone in the game: OBJECT_STATE_DEAD (objects_kill, _kill_flagged,
  _kill_group): left in place, its slot drawn as nothing
  (objects_instance_model -1), objects_find_id -1, no body, not hit by
  rays, not saved. The list is compacted (objects_compact_dead) only in
  world_save (the editor's save, the game's). The physics' removals, the
  harvest's, the explosives', potop_object_remove and the ground items'
  pickups kill; the editor's deletions still take out (and sort).
- objects_t.changes (object_change_t: added, killed, freed, retyped, where)
  since the last sort, read with objects_changes_since(serial): the
  physics (follow_changes: bodies made / taken away) and the navigation
  (the regions within reach marked objects_touched, compared again) go on
  from their serial; a sort begins it again (a new generation: they go
  through it all as before).
- Those who go by the cells also go through the strays and those added
  since: flush_slots, find_cells (the bound, the models in view),
  objects_visit_circle, objects_raycast, the navigation's gather; the
  cells' readers skip the loose and the dead.
- flatten_by_water returns at once without a type of physics = flood.
- The smiths: the map scanned when potop_objects_generation changes (a
  map loaded, the editor), those whose smithy is gone every 5 s.
- 38k: the pass after the focus moved 64 m goes by the grid
  (range_by_cells): the cells round where it was (DROP) and where it is
  (RANGE), at most BODIES_LOOKED_AT_PER_STEP objects and 2 ms of making a
  step, then the loose, the added and the strays; not the whole list
  (millions: 3-7 ms a step for some 20 steps). After a sync that ran out of
  time it still goes by the list (bodies_cursor).
- 38l: the valley fog's froxel history: fog.c keeps the frame before's
  matrix from the eye (the inverse of inverse_view_projection_relative),
  fog_inject.comp looks the froxel up at middle - fog_previous_eye. With
  38f the froxels were placed by the precise rays and looked up in the
  frame before by the absolute matrix (inverted from an inverse, kilometers
  out): a little off every frame, the history smeared into a second fog.
  The sun on the screen for the fog's rays from the same matrix.
- 38m: no range any more (38h, 38k gone): bodies are made a grid cell of
  the objects (32 m) at a time when something wants them there, and stay.
  physics_t.cell_states (0 not yet, 1 queued, 2 made), cell_queue (made by
  make_queued_cells, 2 ms of making and BODIES_LOOKED_AT_PER_STEP objects
  a step, the head cell from queue_cursor). Wanted: every step the cells
  within BODIES_RANGE_METERS (1 km) of the focus, those within
  BODIES_AT_ONCE_METERS (96 m) made at once (a jump), the cells round each
  active water tile, round each actor (world_simulate:
  physics_request_bodies, PHYSICS_ACTOR_BODIES_METERS 48 m). sync_bodies
  and follow_changes make bodies only in made cells (in_made_cell; no
  focus, the tests: everywhere); a body once made is never taken away for
  distance. A new map (physics_reset): no cell made.
- 38n: the loose bodies are in their ground tile's list too (moved to the
  new one as they go: the move events), and the loose list is gone: the
  water's push looks at the bodies of the tiles where the water moves and
  at those that moved the step before (physics_t.awake_slots, copied from
  moved_slots in restore_poses), each once (visit_stamp). With every
  boulder and barrel within a kilometer made (38m), going through all the
  loose ones every step was 4 ms with no water at all.
  wake_bodies_in_rectangle finds the loose ones by the tiles' lists.

## The culling looks at the visible cells only (38o)

objects_cull.comp ran one thread per GPU slot (every object on the map),
in COUNT and in SCATTER, for each pass: the only cost each frame that
grew with the whole map. Slots are fixed per object (take_slot), not in
the grid's order, so the GPU could not be given "slots a to b". Now:

- objects_t.order_buffer: per instance, in the list's order (sorted by
  cell, then draw distance rank), its slot; ORDER_NONE where there is
  none or for a stray where the sort left it; after instance_count, the
  strays' slots. update_order: whole after a sort (sort_serial, counted by
  clear_strays: the sort and forget_strays), the tail (from sorted_count:
  those added since and the strays) when instance_count or stray_count
  changed. 4 bytes an object.
- find_cells, besides the visible bits, makes the pass's ranges of the
  order (add_order_range, joined when one goes on from the one before):
  each visible cell's [first, last) cut at its draw distance (clamped to
  sorted_count), the loose ones after the cells, the tail. Up in
  range_buffers[pass] (order start, work start). The far shadow map:
  every cell, so about the whole list, as before.
- The shader: one thread per item of work (work.x), slot_of_work finds
  its range by a binary search, the slot from the order. visible() is the
  same as before. A stray (strays/stray_slots, each once: stray_marks per
  instance) has FLAG_STRAY in its slot's cell word (pack_slot): skipped
  where the sort left it (order index < work.z = instance_count), looked
  at in the tail. clear_strays touches the strays' slots (by stray_slots:
  the indices move when compacted) so they go up without the flag.
- Bindings: 8 read-only buffers, SDL_GPU's limit (MAX_STORAGE_BUFFERS_PER_STAGE;
  a 9th: the pipeline refused, nothing drawn, the first 38o): the order is
  7, the ranges are in the visible cells' buffer after the bits (from
  work.w = cell_visible_words; cell_visible_capacity_bytes). cull_uniforms_t
  304 bytes (work[4]).
- Before 38o: engine/objects.c, objects.h, objects_cull.comp kept in
  /home/claude/backups/pre_38o (the container's; the zip before is 38n).


## Ambient occlusion from the depth (38r)

ambient_occlusion.c/.h, ambient_occlusion.frag, ambient_occlusion_apply.frag.
GTAO (Jimenez 2016) for the scale under the sky occlusion's 0.5 m texels.
Drawn in world_render.c after the scene's opaque things and the sky, at the
render_target_pause before the water, before the contact shadows (both
multiply the color: GPU_BLEND_MULTIPLY).

- Step 1, half size (R16G16F: occlusion, distance from the eye, -1 sky):
  3 slices x 5 steps x 2 ways over the full size depth, a 4x4 pattern
  for the slices' turn and the steps' offset, moved each frame over its
  16 places (since 38t: the history gathers them). All from the
  eye (clip_to_eye). The slice's direction in the world: the point 4
  pixels aside on the screen at the same depth, made square to the view.
  Radius render.ambient_occlusion_meters, at most 160 scene pixels on the
  screen, nothing under 3; falloff over its last 60 %.
- Step 1b (38t), half size: ambient_occlusion_temporal.frag, this
  frame's occlusion with its history (history_textures[2] in turn):
  the point (its top left scene pixel, from clip_to_eye) plus the eye's
  move, through last frame's view_projection_relative (kept in
  ambient_occlusion_t); the history's 2x2 round there bilinear, each only
  if its distance is within 2 % + 5 cm of the point's from last frame's
  eye; mix 0.875 history. None: a pause over 250 ms, the eye jumping over
  4 m, a new size. The grass in the shade flickered without it.
- Step 2, full size: the 4x4 half size pixels round it (the pattern
  averaged away), weighted by exp(-|distance - its| / (3 % + 5 cm));
  then only the sky's share of the light taken away (as the contact
  shadows work out the sun's: facing, sun_visibility, sky and ground
  bounce), faded out toward render.ambient_occlusion_distance.
- Settings (render.): ambient_occlusion (true), ambient_occlusion_meters
  (1.0), ambient_occlusion_strength (1.0), ambient_occlusion_distance
  (150 m); in both settings tables (settings_window.c, ui.c), live.
- No shadow maps (shadow_strength 0): the depth bound in their place,
  never read (parameters.w 0: the sun reaches everywhere).
- The sky occlusion stays: the big scale, off screen, far, and the fog,
  the particles, the water, the grass and canopy_cover read it.

## A new map's start (38s)

- layouts_populate_map: the layouts' circles to clear of plants are
  gathered (place_collecting adds to one place_context_t's clear list)
  and objects_clear_vegetation runs once at the end. After each layout it
  sorted and compacted the whole list (a million objects, 390 layouts on
  a map of 4 km: 25 s). Nothing in the placing looks at the plants;
  layouts_place (one layout, the editor) still clears at once.
- world_populate: navigation_forget, not navigation_build_all (5 s on a
  map of 4 km): every region built when first gone through, as on a
  loaded map.
- Sky occlusion at the start (6.3 s on 4 km, 128 directions): GPU work,
  render.sky_occlusion_directions_per_frame (4) directions a frame, each
  a depth map of 8192 texels with the whole map's terrain and objects,
  then 16M cells: a frame of 200 ms or so and the desktop froze (no GPU
  preemption). Not changed yet; see TODO.

## Bounced light with the ambient occlusion (38w)

Screen space indirect light, the horizons' way, in ambient_occlusion.frag
(render.indirect_light, render.indirect_light_strength; both settings
tables, live):

- Step 1 reads the scene's color so far (opaque things and the sky, lit,
  before the water and the fog: color_texture, bound as a sampler while
  the half size targets are drawn). Each sample that raises a slice's
  horizon adds its color times the cosine weighted arc it newly hides
  (slice_arc(before) - slice_arc(after)) times projected_length; / slices,
  held under 16. Out: a second target (RGBA16F, extra_color_target_count
  1 in make_pass).
- Step 1b averages it with its own history (indirect_history_textures,
  the same rejection and weights as the occlusion).
- Step 2 gathers it 4x4 as the occlusion, then the multiply factor gets
  + indirect / lit, lit = sun_color * facing * mapped + (sky_color +
  0.3 ground bounce) * ambient (per channel; at least 0.02), held under 3:
  the pixel's color times that is its albedo times the bounced light (no
  surface buffer needed). Faded with the occlusion's distance.
- Not known: the found point's facing (no normals there): a wall's back
  lights too. Only what is on the screen sends light.

## The bounced light, fixed (38y)

- ambient_occlusion_apply.frag: the pixel's own light from round about as
  prop_body.glsl's surroundings give it, mix(ground_bounce_color, sky,
  normal.y * 0.5 + 0.5) * ambient: facing down (a stone's foot) it is the
  ground's bounce, not 30 % of it; both the occlusion's share and the
  bounced light's divisor use it. The gain held in [-0.5, 1.5] (was 3:
  the orange rings round stones on sunlit ground).
- ambient_occlusion.frag: the bounced light has its own radius,
  render.indirect_light_meters (3 m), with BOUNCE_STEPS 3 more steps
  beyond the occlusion's radius (up to BOUNCE_MAXIMUM_PIXELS 320) and its
  own horizons. Beyond the occlusion's radius a found point counts only
  its light minus what the shaders took for that direction (the ground's
  bounce below, the sky above): the occlusion did not take that arc away,
  so the full light would count it twice; the difference is the tint (a
  green meadow over a plain ground), and can be below 0.

- 38z: a facing not to be had (the neighbors in a line: a blade of grass edge on) made normalize() not a number in step 1, step 2 and the contact shadows; kept in the history it showed as white or black spots (the screen's lower corners, grass at the feet). Guarded (cross length), the temporal pass and step 2 skip a not-a-number; step 2 fades in from 0.4 to 1.2 m from the eye.

- 39a: within the occlusion's steps the bounced light weighs each point by the occlusion's falloff, not its own: by its own (3x the radius) a point 0.4..1 occlusion radius off gave back all its light where the occlusion took only part, and under the eaves the bounced light undid the occlusion.

## The bounced light, taken out (39c)

The screen space bounced light (38w..39a) is gone: it cost frames, made
the feet of stones glow and the terraces' ceilings change with the camera
(only what is on the screen sends light; downward faces have little light
of their own to divide by), and looked worse than the occlusion alone.
Kept from that work: step 2's light from round about as prop_body.glsl
gives it (the share), the not-a-number guards, the fade in from the eye.
The render.indirect_light* settings are gone.
