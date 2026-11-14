# How it fits together

| File | What it does |
|---|---|
| `main.c` | window, main loop, fixed step simulation, command line |
| `input.c` | SDL events to game actions (the only place that knows about mice and keys) |
| `console.c` | the console: the log kept from SDL_Log, the line being typed, history, Tab completion (main.c runs the commands) |
| `camera.c` | orbit camera, picking rays |
| `terrain.c` | height field (integer units of 0.01 mm), layers (bedrock, soil, silt), chunk bounds, raycast |
| `terrain_gen.c` | warped fBm + ridged mountains, thermal erosion, gullies and buttresses carved into the steep faces (`[terrain] gully_depth_meters`, 3; 0: none), soil classification with pockets of earth left up the rock where it is gentler over a couple of meters (`mountain_soil_pockets`, 0.5; 0: none: grass, and trees whose `spawn_height_maximum` lets them, grow there), bedrock |
| `terrain_cache.c` | saving and reusing generated terrains |
| `water.c` | the CPU's side of the water: its copy, sources (springs, rain, brush), substeps, the inspector's rules |
| `water_gpu.c` | the water simulation on the GPU (virtual pipes, sediment transport, tiles), the terrain's textures made from its ground (water_ground.comp), and the copies to and from the CPU |
| `erosion.c` | the ground's moisture (wetting and drying); runs the erosion, deposition and bank collapse on the GPU |
| `soils.c` | soil properties from `soils.ini` |
| `stamps.c` | terrain stamps |
| `scenes.c` | ready made test scenes (the dam) |
| `water_renderer.c` | water surface: depth color, reflections, ripples, foam, mud |
| `shadow_map.c` | sun shadow map |
| `tools.c` | brushes: raise, dig, paint soil, smooth (and the tool list, with plant and clear) |
| `object_model.c` | object models: the game's own `.pobj` (format in `object_format.h`) and Eternal Lands' `.e3d` |
| `dds.c` | DDS texture decoding (DXT1/3/5 and uncompressed) |
| `plant_atlas_file.h` | the plant atlases' file (a BC7 DDS) |
| `season.h` | the year in the plants: the month, the autumn and the snow |
| `object_types.c` | object types from `objects.ini` |
| `objects.c` | placed objects: grid, standing on the ground (or floating), culling, fading, drawing |
| `populate.c` | vegetation: populating the map, the plant brush |
| `layouts.c` | layouts from `layouts.ini`: placing, levelling, new maps |
| `physics.c` | objects as Box3D bodies: buoyancy and drag from the water, anchors, damage, flood behavior of plants, ground tiles, water walls |
| `terrain_renderer.c` | chunk culling and level of detail, GPU uploads of changed chunks (wetness, blurred soil weights; the heights, normals and surface too when the GPU cannot make them), drawing |
| `render_target.c` | offscreen scene buffer (linear light, half floats), for `render_scale` |
| `tools/potop_plant.c` | grows trees and small plants into `.pobj`, keeps the plant atlases (BC7, each picture's months in cells of their own) |
| `post.c` | the frame's last pass: exposure, bloom, tone mapping (HDR) |
| `daylight.c` | day and night: the sun's path, the moon, the sky's colors by the hour |
| `gpu_timer.c` | GPU time per render pass (a stub until SDL_GPU has timer queries) |
| `gpu.c` | the SDL_GPU device, uploads, samplers, pipelines, the frame (scene, interface, window, screenshots) |
| `scene_uniforms.c` | the scene block every lit shader reads (camera, sun, fog, shadows, clouds, under water) |
| `materials.c` | texture loading, procedural fallbacks, normal maps, texture arrays |
| `material_map.c` | the material map: the bases baked from the terrain's rules, the objects' cover and shelter, the floods taking the cover |
| `ground_overlay.c` | snow and petals on the ground, in amounts, on the GPU (ground_overlay.comp) |
| `noise.c` | simplex and periodic noise, fractal sums, PRNG |
| `settings.c`, `ini_parser.c` | settings with defaults, INI parsing |
| `file_io.c` | all file access, through SDL's IO layer (Android ready) |
| `particles.c` | Particle effects: data/particles.ini, the textures, the GPU passes (see Particles) |
| `screenshot.c` | PNG screenshots |
| `ui.c` | the interface: brush panel, icon bar, status line, settings, diagnostics, maps, sky, console (drawn) |
| `ui_backend.c` | Nuklear on our side: font baking, drawing, SDL events and which ones the interface keeps |
| `map_file.c` | saving and loading maps (zip archives through miniz) |

A few rules the code follows:

* No `fopen`: all file access goes through `file_io.c` (SDL_IOFromFile), so
  assets can later be read from inside an Android APK.
* Heights are integers so the water simulation can move material between
  terrain, sediment and water without rounding losses.
* The terrain mesh is one 64x64 grid reused for every chunk (at several
  vertex spacings, for the level of detail); heights live in a float
  texture, so editing the terrain only re-uploads the touched chunks.
* Tools run in the fixed simulation step (30 Hz by default), so a brush stroke
  does the same work at any frame rate.
* Water is conserved exactly: depths are integers, and each pipe's flow is
  turned into an integer amount once, which leaves one cell and enters the
  other. `--water-test` checks this to the last unit. The same goes for the
  ground: every unit of material is in the ground, in suspension, or gone
  over the map edge, and `--dam-test` checks that too.
* Only tiles with moving water are simulated; still water sleeps, dry tiles
  cost nothing.
* The water runs on the GPU, in compute shaders (`data/shaders/water_*.comp`,
  see `water_gpu.h`): one workgroup per tile, the tiles to work on listed
  by the GPU itself (active ones, then the neighbors they wake), so nothing
  waits for the CPU. The cells are stored tile by tile, each tile one block.
  The renderer draws from textures the simulation writes (the depth after
  the last step and the one before: the surface is drawn in between, by
  the time since the last step, so it moves smoothly at any frame rate
  although the simulation steps 30 times a second); the CPU keeps a
  copy of the water for the physics, the actors, the navigation and the
  inspector, brought back each frame for the tiles that changed (those
  around the camera's target first, up to about 10 MB a frame), so it is a
  frame or two behind. What the CPU changes (stamps, scenes, loaded maps,
  the walls of houses, edited terrain) goes up tile by tile. Saving a map
  and the tests copy everything back first.
* Erosion, deposition, bank collapse and smoothing run on the GPU too
  (`water_erode.comp`, `water_collapse.comp`), on the tiles with flowing
  water, mud, or a recent change. The ground they change comes back to the
  CPU a few dozen tiles a frame, for the terrain's mesh, its physics and the
  shadows.
* The ground's moisture is not part of the water any more: it is a coarse
  number kept on the CPU (for the farms, the dark wet look and the game's
  rules), updated every 10 s, a share of the tiles each step. Ground under
  water or in the rain gets wet; it takes no water away. Rain adds no water
  either: its drops will be particles.
