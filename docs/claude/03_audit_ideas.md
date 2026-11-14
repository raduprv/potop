# Audit: optimization candidates and eye candy

Written in a 30 minute pass (November 2026), from reading the code, not
from profiling. Nothing here is decided; discuss with Radu before starting.
The engine is already careful: dirty flags and per-frame budgets almost
everywhere (chunk bounds, objects following the ground, shadow maps redrawn
only on change, near shadow in still and moving layers, water tiles asleep,
GPU culling with Hi-Z and indirect draws, impostors and BC7 caches). So the
honest first step for performance is numbers, not guesses.

## How to measure (ask Radu, on his GPU)

- Console `profile` (ms per FRAME_* section since the last call) and
  `profile frame` (one frame step by step). A big `acquire` means the CPU
  waits for the GPU (GPU bound); otherwise CPU bound.
- gpu_timer.c is a stub (SDL_GPU had no timestamp queries); check whether
  SDL 3.5 has them now: real per-pass GPU times would make the GPU side of
  this audit possible.
- Lock GPU clocks before comparing GPU times (NVIDIA lowers clocks when the
  frame is CPU bound; see TODO.md, Performance notes).

## Optimization candidates

1. Load time on weak GPUs: impostors bake every model with one at load
   (97 models, 112 s on lavapipe; cached after) and sky occlusion works out
   64 directions (113 s on lavapipe for a 1024 map). On a real GPU these are
   probably seconds, but on Android they matter: check the cache is reused
   across runs and maps, and whether the sky occlusion can be saved with
   the map (map_file) instead of worked out at load.
2. Actors at scale (TODO.md already has the plan): instanced drawing with
   poses in a texture, far actors' poses updated less often, path searches
   spread over frames or threads. Measure with 1 000 / 10 000 bears first.
3. Navigation: a region rebuilt while erosion changes the ground walks every
   object of the map (TODO.md); keeping each region's list of big objects
   removes that.
4. objects_sort: a calloc of (cells + 1) x types ints and a full counting
   sort each time an object is added or removed (grid 32 m cells; a 1 km
   map with ~100 types is ~400 000 ints). Fine for edits; if the game ever
   adds and removes objects every frame (ground items, felled trees,
   debris), keep the bucket array and sort only the changed cells.
5. Generated buildings: the workshops are 5 000 to 12 000 triangles at full
   detail with many small pieces (rubble 40 stones, log piles). Their
   MEDIUM level already drops the bevels; check in `--houses` how far FULL
   is drawn with the lod_errors floor, and whether rubble and small props
   could go to one merged low piece earlier.
6. Shaders: building.frag / prop_body.glsl do aging, moss, snow, wetness,
   sky occlusion, ground blending per pixel. If the objects pass shows up in
   GPU time, a cheaper variant for far levels (no relief, no moss detail)
   picked by LOD level is the usual win.

## Eye candy that fits what exists

Ordered by value for effort, roughly.

1. Smoke and fire on the new workshops: object_effects.c already plays
   effects written into models (forge_fire, forge_smoke, chimney_smoke in
   particles.ini). The smelter's bloomery, the charcoal burner's clamp
   (thin blue-white smoke all over its skin), sparks at the smithy. Mostly
   generator work (anchors with effects) plus maybe one new particle type.
2. Contact ambient occlusion for objects (GTAO or simple SSAO at half
   resolution): nothing small scale exists; sky occlusion is 0.5 m texels
   on the ground. The Hi-Z depth pyramid already exists (hiz.c) and the
   scene depth is a texture: a half-res AO pass after the scene, applied to
   the sky light only. Biggest visual gain for buildings standing on the
   ground and props in yards.
3. Lightning in storms: cloud_weather.c has a "storm" family and rain
   exists, nothing flashes. A flash = a few frames of strong light from
   inside a storm cloud (the cloud march can add an emissive point), a bolt
   as a jagged strip drawn once, the scene's sky light lifted for 100 ms,
   thunder delayed by distance later when there is sound.
4. Puddles after rain: the terrain already has wetness. Hollows (negative
   curvature from the heights, or where the water simulation left a few mm)
   get a flat mirror-like layer with the sky reflected and ripples while it
   rains. Cheap version: darker, glossier, sky reflection; no simulation.
5. Heat shimmer over fires, the smelter, the forge: a screen-space
   distortion of the scene color in post, masked by a few world-space
   volumes the effects give (there is already a shimmer in the water and
   aurora code to borrow from).
6. Specular occlusion: the sky occlusion value also damping the sky's
   reflection on buildings (wet roofs, glass) so inner corners do not
   shine. A couple of lines in prop_body.glsl / building.frag.
7. Watermill alive: spray particles where the paddles hit the water, a
   splash curtain, a wet dark band on the stone under the wheel; once the
   river exists, foam downstream (the water renderer has foam).
8. Birds: flyers exist (falcon); a small flock circling over the windmill or
   the church tower at dusk is cheap with boids on the CPU (a dozen).
9. Depth of field for dialogs and the map/journal overlays: blur the scene
   behind the UI. Pure post effect, small.

## Debt noticed

- TODO.md lists finished work as planned (water on the GPU, SDL_GPU, Hi-Z
  culling). Worth a cleanup pass so it stays useful.
- README.md's module table predates about 40 modules (listed in
  01_engine_map.md).
- settings.ini has an unknown key: `[render] height_blend_depth` (line 65)
  is warned about at every start.

## Valley fog's cost (noted 35b, not done)

At fog_form_hour (19:00 by default) fog_amount_at_hour goes over 0 and the
whole volumetric pass (inject, integrate, rays, apply) runs: about 1.3 ms
on Radu's machine (220 to 170 fps), even while the fog is too thin to see
(it grows over four hours; on the houses map it did not show at all). Ideas:
start it over a small amount (0.05), the simple mode while it is thin,
volumetric only once it is dense. Radu: leave it for now.
