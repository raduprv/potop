# Potop: to do

Ideas and plans discussed but not done yet. Rough order within each part:
what seemed most worth it first. Edit freely.

## Modules (37f: started)

- Save a module's state in the map (now placed in its first state).
- Rope stretching: a scale along one axis per instance (a 12th instance
  float in every object shader), then the quarry's rope in one piece.
- Parts parented to parts (a hook on a jib that turns).
- The editor: moving a module moves only its body (re-place the parts).
- Animal spots in modules (homes found by module), building scripts that
  drive the states, the worker that makes them run (Gigel in the wheel).
- Hidden parts with collision keep their body.

## Workers (37j: the smith)

- Props in hands: attach objects to actor bones (hand_R_prop / hand_L_prop:
  the hammer, the tongs; the tongs change hands between forge and anvil).
- The forge's fire brighter while the bellows blow (its light, the embers).
- The smith stands just outside the shed at the bellows' handle: move the
  bellows in a little, or the post.
- Foot IK bends a human's knees wrong (blacksmith: ik = false for now);
  ground_align must be 0 for people.
- Stairs at the cabins' doors (log cabin floor 0.45 m up: none yet).
- His day (37w: home, the inn, a schedule): no interiors yet (he goes in at
  a door and is gone); the inn's terrace; not saved with the game yet (a
  load puts him where his day wants him); a rest day; other people
  (innkeeper, customers); the player talking to him.
- Long ways: waypoints in the map editor with their ways baked between
  them; at run time only the way to the nearest (3-4 tried) and from the
  last; map tiles streamed (baked maps only) for maps over 4 km; camera
  relative drawing past 4 km.
- Actor textures: bleed and BC7 in the converter, not at every start (a
  4096 x 7200 atlas took seconds: zlib + 16 bleed passes).
- Humans' textures (blacksmith.pact 4096 wide): fine for a test only.

## Roadmap (agreed order)

1. Everything onto the GPU, in sub-steps tested one by one:
   a. SDL_GPU instead of OpenGL: shaders stay GLSL, compiled to SPIR-V with
      glslangValidator and committed to the repo (`make shaders`); Linux
      and Android are the targets, Windows comes along on Vulkan with the
      same SPIR-V, Mac gets MSL from SDL_shadercross (built without DXC)
      on a best effort basis. Uniforms go into blocks, bindings follow
      SDL_GPU's sets. Headless tests run on Mesa lavapipe (Vulkan 1.4, apt
      package mesa-vulkan-drivers).
   b. Water and erosion on the GPU (compute). The GPU holds the truth; the
      CPU keeps a mirror read back asynchronously, only for changed tiles
      (physics, navigation, objects, saving). Tools write small rectangles
      to the GPU. The CPU water code stays only as the reference for tests
      until the GPU one matches, then goes. The water's forces on bodies
      can move to a compute pass too.
   c. Occlusion culling (Hi-Z from the depth of the frame, two passes,
      indirect draws).
2. glTF 2.0 import in tools/potop_convert (cgltf): static objects first,
   then actors with skeletons and animations (models from free asset
   sites and generators). Materials: base color first; then measure on a
   real GPU (not llvmpipe) what a full PBR shader costs against base color
   only: same seed and fixed camera from the command line, a big dense
   forest up close at full resolution, comparing the GPU time of the
   objects pass (not fps, the game is CPU bound). One shader with default
   textures (a flat 1x1 normal map and such) unless that shows a real cost.
3. Map editor, as a mode of the game (one code path): every change is a
   command (place, move, delete, set interactive, brush stroke), which
   gives undo/redo and becomes the first draft of the engine's API.
   Needs stable object ids. An object browser with 3D thumbnails
   (rendered once, cached on disk, by category, with search).
4. Painted maps: a few small layers (terrain classes, vegetation, water,
   settlements) painted in a minimal in-game painter (or imported as
   PNG); the generator fills in the detail, blending classes smoothly at
   their borders.

Saving: until the final version maps are saved as compressed snapshots
(map_file with miniz), completed with actors, effects, the interactive
flag and stable ids. At the end: maps as recipes (seed, size, painted
layers, object changes with their final state; a version number, and maps
from an older version get their terrain difference baked in once).

## Navigation mesh (Nov 2026: phases 1-3 in, navigation.h)

- The small class at 0.5 m: the region build rasterizes objects into 1 m
  cells; a 0.5 m grid for the small class (hens through fences, people in
  narrow rooms) needs that build at 0.5 m first.
- Phase 4, memory for big maps (8k): the per cell grid only while a tile
  is built (heights from the terrain, feet on objects from the tile's
  surfaces); the mesh (and the flyers' coarse grid) kept. Baked maps save
  the mesh with the map.
- Phase 5, any number of levels: the build reads the objects' collision
  straight into columns of surfaces (now 3 above the terrain at most).
- Then the cells' A* and the path pulling can go.
- A running cow (3.3 m/s, 30 degrees a second: a turn 6 m wide) on a
  terrace 2 m deep goes round in circles at the rail (the step guard
  keeps it in): it should slow to a walk where its turn does not fit.
- Church porch: the farmer (1.85) under its 1.65 m beam (see below).
- Stress test (tools/nav_stress.py run) known limits: ground cells are still whole meters (a 1 m
  door on bare ground can come out shut); the portals to the next tile
  are not cut to their quarter meters. Each fix: a station for it.
- Church porch: a beam 1.65 m over the top of its stair: the farmer (1.85,
  his model's height) no longer goes up. A height_meters for people
  (stooping) or the model's beam higher: Radu's call.

## Particles

- Preview window: pick an effect, play it in front of the camera, change its
  values with sliders and see them at once (after we have played with the
  particles for a while).
- Sorting the lit (smoke) particles back to front on the GPU: unsorted, two
  smoke columns crossing can show the wrong one in front.
- Soft particles against everything (objects too), not just the ground and
  water under them: needs the scene's depth as a texture.
- spawn_on_death: a particle that dies starts another effect there (flames
  turning into smoke, raindrops into splashes).
- Particles carried by the water's flow (once the water is on the GPU).
- Day and night, wind (trees swaying): talked about with particles, not started.

## Water

- Check: far water uploads now wait only beyond far_upload_meters (300 m)
  or off screen; see whether that is enough.
- Water at the foot of high cliffs: deep water reported at 50+ m/s while it
  looks like 10 m/s. Substeps no longer follow it past
  substep_speed_meters_per_second (10 m/s); find why the speed is that
  high (no friction on steep slopes?) before the water goes to the GPU.
- Unfreeze only the body of water that was touched (flood fill over the wet
  tiles from the edited one; a frozen body the running water reaches wakes
  whole), instead of the whole map.
- Waterfalls that look like waterfalls: foam on steep slopes, a splash of
  particles where the water lands.
- Speed: the passes are vectorized; what is left is memory bandwidth.
  Candidates: tile-major memory layout, narrower data in the water loops
  (16 bit ground, obstacle and soil in one byte), 0.5 m cells as an
  experiment (needs the lip, thin water, shore and wake thresholds retuned).
- Water on the GPU (compute shaders, PC only): fluxes and depths without
  uploads; read back what the physics, saves and inspector need. Big; only
  if the CPU is not enough.
- Inspector: show which tile keeps a tile awake, with a button to go there.
- Less opaque mud on waves.

## Sky

- Sky presets: cumulus, cirrus, overcast, storm (dark bases, with the rain),
  with a slow blend between them.
- Big separate clouds with volume, 2.5D: thickness maps per cloud, the
  sunlight inside them baked at load (the sun does not move), the view ray
  marched through the cloud layer in real time with a forward scattering
  phase function (dark with silver edges towards the sun, white away from
  it); their shadows from the same maps.
- Physical sky (Preetham or Hosek) if the sun ever moves (day and night).

## Under water

- Bubbles, a wobble of the view, an effect on the waterline when the camera
  is half in the water.

## Game

- Sound: running water louder where it is fast, waterfalls, rain, splashes,
  banks collapsing.
- First person: a crosshair, heavy loose things (a log) slowing the walker
  instead of being walked through, standing on loose bodies (a raft or boat
  to ride down a river), picking up and throwing logs.
- Actors: saving them with the map, chained actions (sit_down then
  idle_sit, instead of back to idle), actors avoiding each other, a
  swimming animation, actors wider than a cell (a path through a 1 m gap
  for an elephant), pushing and being
  pushed by physics bodies (a kinematic capsule: logs, steps; a body that
  touches it calls actors_mark_path_dirty).
- Stress test: 1 000 and 10 000 actors walking about (city, wildlife):
  frame time split into moving, poses, drawing, path searches. Likely
  fixes: drawing instanced with the poses in a texture (decided: at least
  for wildlife), poses of far actors
  updated less often (or frozen), searches spread over frames or threads.
- Flyers: a sitting animation for birds on the ground (the falcon plays its
  fight idle); perching on trees and roofs; flocks; a flyer's ceiling and
  heights from the game (a hawk over a valley, a duck low over water);
  landing on water for water birds.
- Feet: the toes do not bend (a paw on a rock's edge); actors on a
  bridge's last cell dip for a moment (the body's height there comes from
  the trench's wall); smoothing the body's height when the feet's ground
  jumps (stepping onto a rock).
- Navigation: paths through forests zig-zag (the pulled lines must miss
  every blocked cell); an actor's height on a bridge goes in steps of a
  cell (smooth it); a region rebuilt while erosion changes its terrain goes
  through every object of the map (keep each region's list of big objects);
  a search costs about half a microsecond per surface (keep what it worked
  out about each node for the rest of the search, or threads).
- Shadows of moving actors without redrawing the whole near shadow map:
  a small shadow map of their own, or drawn over a copy of the static one.
- glTF to .pact in the converter (cgltf), once there is a model from
  Blender to try it on. Trees swaying in the wind in the vertex shader (no
  skeleton needed).
- Playground mode (real water) and game mode (frozen or fake water, flow
  maps).
- An engine API, so a game is put together like lego, without math in the
  game code. The game has its own main and includes the engine as source,
  not as a linked library. Actors: add_actor, remove_actor,
  move_actor_to_position and similar, with the engine doing the pathfinding
  and making the actor walk, swim or fly there. Needs careful planning
  before any code (changing terrain and water, coarse navigation grid,
  currents pushing swimmers, events back to the game).

## Objects

- LOD for objects with many triangles (not terrain): levels generated at
  load with meshoptimizer (meshopt_simplify; C++ with a C header), hand made
  LOD files override them when present. Pick the level by size on screen,
  with hysteresis and a short dither crossfade against popping; the shadow
  map uses one level lower. Only once there are real models; the current
  ones are placeholders.

- Windows by the hour (objects_light_windows): a list of the objects whose
  models have emissive window groups (made at the sort, the tail and the
  strays added), not a pass over every object (OBJECTS_WINDOWS_PER_FRAME
  100000 a frame, from a game minute's change: 15 real seconds at the 1/4
  day). Then the changes can come more often than once a game minute (each
  house its own time to light up, a light going out late), and the cost
  grows with the houses, not with the map. Can also be limited to the
  houses near the eye (the grid), the far ones caught up as they come near.

- Sky occlusion at a new map's start froze the whole desktop for its 6 s
  (the GPU taken whole, 4 directions a frame of 8192 texel depth maps):
  kept with the baked map or cached on disk (by the terrain's and the
  objects' hash), and/or fewer directions a frame.

## Buildings and the village (Nov 2026)

- BUG: impostors (and far buildings and trees still drawn as meshes)
  float over hilltops and sink in valleys far off: they stand on the real
  heights, the terrain is drawn from coarse LOD chunks (every other vertex
  dropped per level) and only geomorphs back up as you come near. Seen
  best through the spyglass. Fix (agreed, not done): in objects_cull.comp,
  once per instance, the height the terrain is DRAWN at under it (the
  chunk's level by the same rule as collect_visible_chunks: nearest point
  of the chunk with lod_eye_height; the step's quad, its corners morphed
  as terrain.vert does, the same diagonal), minus the real one, as a
  vertical offset for the whole instance; the shadows the same.
- Buildings with state, one mechanism: a per instance fill level for a
  part (the corn in the crib, grain, hay, firewood: vertices above the
  level squashed down onto it in the vertex shader, triplanar so the top
  looks right; or scaled from the base for heaps) and parts shown or
  hidden by a per instance bit mask (log stacks in steps, sacks, laundry,
  open/closed shutters as two variants). Game API: objects_set_fill,
  objects_set_part. Moving doors (a part turned round a hinge anchor)
  later. Impostors keep the baked state (fine far off).
- Places on the buildings for the people: work spots (anvil, forge,
  bellows, quench tub, bench, loom...), each a position, facing and what
  is done there, written by the generator like the anchors, so a smith
  works round his smithy instead of standing in front of it. Agreed: no
  scripting language on buildings. A building is data (its spots, its
  parts with state, what it uses and makes); the behavior lives in the
  trades (a "smith" is C code that knows what to do at spots tagged anvil,
  forge...), so every generated smithy works; a building's own logic (the
  mill turns only with water, the crib empties in winter) a C callback per
  building type. Later: the buildings' "AI" built on this.
- New buildings: a cemetery (crosses and stones leaning with age, a
  fence, by the church). (The apiary: done in 36w.) The bees fly at night
  too (the effect has no day/night yet).
- Window lights: the pool of light on the ground in front of a lit window
  is faint (dark grass two meters down); maybe a ground-only boost.
- Windows: interior mapping (fake rooms behind the glass) instead of the
  flat glowing texture.
- Glass is passable now (an open door's dark let no cow in): only the
  houses regenerated since 36u have it (cow_farm, hen_house). Regenerate
  the rest some time (no hurry: it matters only at open doors).
- Animals (seen in the --sim trace): hens walk into the water trough
  (its top within their step_meters, so the body rides up onto it and
  the casts go over its sides: step_meters 0.1 for hens, or a code rule);
  sheep mill a while at the strunga at dawn. Each new kind of AI: run the
  trace before the zip.
- Animals (docs/claude/animal_ai_design.md): cows on the boredom walk
  (39j). Next: hens, pigs, sheep on it (they still spin: --walk counts
  it); one file per animal (cow.c ...); needs (hunger, thirst, tired,
  bored); grazing as small steps; terrain preferences and avoid areas for
  the wild ones.

## Dependencies

- C++ libraries are fine when they have a C interface. Nothing more exotic
  (no Rust and the like).

## Android

- Project from the SDL3 Android template, a build script, a debug key;
  SDK and NDK installed under home (sdkmanager --sdk_root).
- Touch: a tap is already a click (SDL); still needed: two fingers to
  rotate and pan, pinch to zoom, buttons for water and sponge.
- Anything listing directories inside data/ must not rely on that working
  on the APK's assets; writes go to SDL_GetPrefPath.
- Vertex formats for phones (memory bandwidth): everything is float32 now,
  80 bytes a plant vertex (position + normal + uv 32, tangent 16, plant
  data + snow 32), 60 an instance. Packed when loaded (normal and tangent
  BYTE4_NORM or SHORT4_NORM: SDL_GPU has no 10-10-10-2 vertex format; uv
  HALF2; plant data UBYTE4_NORM; snow HALF4) about 36. Touches every
  object pipeline (object, plant, building, prop, shadows, the shared
  shadow buffers, impostor baking, the editors' previews): one job, done
  and measured on a phone. 8 bit normals may band on buildings.

## Performance notes

- GPU times per pass go up when the simulation keeps the frame CPU bound:
  the NVIDIA driver lowers its clocks. Not real work; compare GPU numbers
  only with the clocks locked.
- More than 4 simulation threads gains nothing on a Ryzen 3700X with
  DDR4-3200: the water is memory bound.
- If needed: upload changed terrain chunks close by less often too, and
  redraw the shadow map less often while erosion works.

## potop_texture: smithy (forge chimney / wall) — on hold

Work in progress, not yet good enough to use. Layers so far: bricks (running
bond, done), lime mortar fallen off with the heat (about 60% of the wall),
soot (first pass). Field stones exist but were not reworked (`--stones 0`
while iterating). `--soot`, `--stones`, `--smear` switch the layers.

- Soot: beside black, the clean brick reads as glowing (simultaneous
  contrast). Dull and darken what is next to the soot (brick most of all),
  do not leave bright orange islands inside it.
- Soot should rise: streaks upward from a few places, not random blotches.
- Then the field stones (now rounded rectangles in the courses).
