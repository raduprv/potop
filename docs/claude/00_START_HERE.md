# Notes for Claude: start here

Written by Claude for Claude's future sessions on Potop. The container is
wiped after a while (a month idle wiped everything once), so these notes
live in the project, not in the container. Radu keeps them; read them at
the start of a session instead of exploring from scratch.

Files here:

- `00_START_HERE.md`: this file. Setup of a fresh container, how to work
  with Radu, the pitfalls.
- `01_engine_map.md`: where things are in the engine, what a frame does, in
  what order; what the manual does not list.
- `02_house_generator.md`: tools/house_grow.c (17 600 lines): materials,
  levels, recipes, the special buildings (church, mills, workshops).
- `03_audit_ideas.md`: optimization candidates and eye candy worth adding,
  with where in the code they would go. Unverified ideas, not decisions.
- `../../side_projects/damage/DESIGN.md`: the destruction model (links,
  pieces, the support pass) as agreed with Radu, its steps, and the soft
  renderer made to look at it without lavapipe.

The manual in docs/manual/ (the big README.md until Nov 14 2026) is good and current for most systems (water,
sky, clouds, objects, actors, plants, props, houses, physics, the material
map, sky occlusion). TODO.md is partly stale: the water and erosion on the
GPU, SDL_GPU and Hi-Z culling are done although it still lists them.

## How to work with Radu

- Reply in Romanian, relaxed, short, a joke or two; no flattery. When he is
  wrong say so bluntly. Game text (UI, logs, comments) stays in English.
- C11, Allman braces, tabs, long snake_case names, no function-like macros,
  English comments in the style already in the code (plain sentences,
  parenthesized asides).
- Wait for "bagă" before big work. "Stai să discutăm" / "nu băga încă"
  means discuss only. Confirming a plan is not a start signal.
- He tests performance and visuals on his own hardware himself. Do not run
  long tests he can run faster, unless he asks (he sometimes does: "testează
  în joc cu --houses").
- His textures and normal maps are already checked by him in the game:
  never edit them, never flip channels. Only resize (to 1024) and make the
  ARM map when asked. If something looks wrong, say so and ask.
- He edits data/objects.ini himself: give him section text to paste.
- Zips: incremental (only changed files), never reuse a zip name (they go
  potop_stepNNx: 34a, 34b... continue from the last one). Send one ONLY
  when he asks (since 34x): first show screenshots, he looks and asks for
  tweaks, then he asks for the zip. NEVER include data/settings.ini, data/materials.ini or
  data/objects.ini. Say explicitly, in capitals, when a zip NEEDS A REBUILD
  (C or shader sources changed). He was burned once by not being told.

## A fresh container

```
# SDL3 from GitHub main (worked with SDL 3.5.0, no API changes needed)
git clone --depth 1 https://github.com/libsdl-org/SDL.git deps/sdl3
cmake -S deps/sdl3 -B deps/sdl3-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$PWD/deps/sdl3-install -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_X11_XCURSOR=OFF -DSDL_X11_XINPUT=OFF -DSDL_X11_XFIXES=OFF -DSDL_X11_XRANDR=OFF \
  -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XSHAPE=OFF -DSDL_X11_XTEST=OFF -DSDL_WAYLAND=OFF
ninja -C deps/sdl3-build install
apt-get update && apt-get install -y glslang-tools mesa-vulkan-drivers vulkan-validationlayers
```

Build through a wrapper, always: without PKG_CONFIG_PATH make fails
quietly and the old binaries stay, which looks like "my fix did nothing".

```
#!/bin/sh
export PKG_CONFIG_PATH=/home/claude/deps/sdl3-install/lib/pkgconfig
export LD_LIBRARY_PATH=/home/claude/deps/sdl3-install/lib
cd /home/claude/potop && make -j$(nproc) "$@"
```

`mk.sh all tools` builds the game (potop), the editor (potop_editor), the
shaders (data/shaders/spirv) and the tools.

## Running the game headless

Radu's rule: switch off in the settings everything the test does not need
(for decals: no shadows, no sky occlusion, no clouds, no fog, no water...),
and use a small map (run_test.sh in this folder: 512 cells). Faster runs,
fewer things to blame.

```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json POTOP_SCRIPT=script.txt \
  xvfb-run -a -s "-screen 0 1280x720x24" ./potop --width 640 --height 360
```

- The container has 8 GB. The default 4096 map on lavapipe (the GPU's
  memory is the process's) is killed by the OOM killer (exit 137, dmesg
  shows "Memory cgroup out of memory"). Set `[terrain] size = 1024` in
  data/settings.ini for the run and PUT IT BACK after (copy it aside).
- A 1024 map takes about 10 minutes to the first screenshot on lavapipe
  (impostors about 2 minutes, sky occlusion about 2 minutes). Run it in the
  background and wait on the log.
- POTOP_SCRIPT: one console command a line; `wait N` waits N frames; e.g.
  `wait 200`, `screenshot`, `wait 20`, `quit`. Screenshots land in the
  project root (screenshot_YYYYMMDD_HHMMSS.png).
- `--houses [category]` lays out a gallery of the generated buildings;
  spinning parts named `<type>_` are placed with their building.
  `--houses name1,name2` (type names, not a category) lays out only those,
  centered in front of the player: the small test scene Radu asked for
  (one or a few objects; much faster frames than the 73-house gallery).
- POTOP_GPU_DEBUG=1 turns the validation layers on (SDL asserts show
  misuse, e.g. an integer texture format used with a sampler: that was the
  cloud weather map crash, fixed in step 34a by making it UNORM).
- The game reloads .pobj files when they change on disk, impostors
  included: regenerating models while it runs is fine.

## Testing the animals' minds: --sim and --trace (36u)

Radu (36t): the simple AI (hens, cows, sheep, bears) came out like a 9th
grader's because I never saw it run. Now I look before I ship:

```
xvfb-run -a ./potop --houses hen_house,cow_farm,stana --width 320 --height 240 \
  --set render.day_cycle=true --set render.day_length_minutes=24 --hour 16 \
  --sim 1200 --trace trace.txt > run.log 2>&1
python3 tools/animal_trace.py trace.txt --out trace.png [--kind ram] [--from S --to S]
```

- `--sim SECONDS`: no picture, no HUD, no impostors, vsync off; fixed
  steps (`--sim-step`, 0.05 s) as fast as they go, then it quits. 1200 game
  seconds (a 24 minute day from 16:00 to past dawn) take about 15 s here.
- `--set section.key=value` (repeatable) over settings.ini; `--hour H`.
- `--trace FILE` (game/trace.c): every animal mind's actor each 0.25 s
  (state, place, moving), their minds' events (walk_short, detour,
  way_in_again, way_out_again, in_late, no_way_in), the homes' spots, and
  the obstacles round them as the walkers' casts see them (at the feet and
  at the middle, potop_debug_obstacles).
- actors' log_paths is on in --sim: run.log has every straight walk that
  ended ("bodies all round", "others all round").
- animal_trace.py prints per kind the time in each state, the events a
  minute, walking in place (moving 3 s, under 15 cm), samples inside an
  obstacle at the feet, how long they took to get in for the night; the
  picture draws it from above (north up). Crop it with PIL to look closer.
- The local data/objects.ini needs hen_house, cow_farm, stana sections and
  actors.ini [ram] (added for the tests; never shipped: Radu has his own).
- tools/animal_test.sh [SECONDS] [HOUR] [OUT_DIRECTORY] does it all (and
  says which ini sections are missing). The test sections used here:
  objects.ini `[hen_house]`, `[cow_farm]`, `[stana]` each `model = <name>`,
  `category = buildings`, `physics = static`, `shape = mesh`,
  `shadow = all`, `random_yaw = false`; actors.ini [ram] as given to Radu
  in 36s (scale 1.8, walk 0.8, cycle 0.55, run 3.5, turn 60, turn_walk
  0.4) and [cow] turn 45, turn_walk 0.4.

## Looking at something close: the camera command (36z)

Console (and POTOP_SCRIPT): `camera <x> <y> <z> [<at x> <at y> <at z>]`
puts the first person eye anywhere (y is its height, even under the
ground or in the grass), looking at the point if given; `look` still
turns it; `camera off` gives it back to the walker. `goto`'s height only
moves the feet (never under the ground), so a low eye needed this. With
`--houses` the log says where each building stands ("Houses: 'X' at (x, z),
turned a half turn": a model point (mx, mz) is at (x - mx, z - mz)). E.g.:

```
printf "wait 100\ncamera 96.2 10.5 108.95 95.89 10.86 109.64\nwait 40\nscreenshot\nwait 10\nquit\n" > shot.txt
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json POTOP_SCRIPT=shot.txt \
  xvfb-run -a -s "-screen 0 1280x720x24" ./potop --houses apiary --width 960 --height 540 --hour 10
```

(lavapipe: about 5 minutes for two shots at 960 x 540; run it in the
background.)

## Fire: effect and burn (Nov 27)

Console (and POTOP_SCRIPT): `effect <name> [x y z]` plays a particles.ini
effect where the screen's middle looks (or at a point), `effect stop`;
`burn` lights the object the screen's middle looks at (engine/burning.h,
data/burning.ini), `burn clear`. A quick look at a burning log on the fast
root (about 90 s): `POTOP_ARGS="--houses log2 --hour 20.5" run_fast.sh
script.txt 640x360 render.local_lights=true render.bloom=true
render.day_cycle=true render.auto_exposure=true render.render_scale=1.0`
with `camera 96.6 10.8 104.9 95.8 10.25 106.2`, `burn`, waits and
screenshots in the script; the fast root's data/burning.ini is a faster
copy (spread 0.25, char 6, burn 40, ash 12).

## Fast runs: /home/claude/potop_fast (35f night)

My own run root, rebuilt in a fresh container with
`python3 docs/claude/night_tools/make_fast_root.py` (copy the night_tools
to /home/claude/tools_night and docs/claude/run_fast.sh to /home/claude
first): its data/ mirrors the project's (files linked), every picture
shrunk to 256 px (atlases 512), its own settings.ini and cache. NEVER zip
anything from it, never put the small textures back in the project.
`run_fast.sh script.txt [WxH] [section.key=value ...]` writes its
settings (fast_settings.py: 512 map, no sky occlusion, clouds, fog, grass,
impostors drawn, reflections, TAA, bloom, lights, decals; render_scale
0.5; sky occlusion still worked out at startup, so 8 directions at 2 m)
and runs from there. Numbers on lavapipe: about 4 s to the first frame
with the caches warm, then a frame about 1 s at 320x180 or half scale, 3 s
at 800x450 (it is bound by pixels). The first run bakes the impostors
(2 min) into its own cache.

## The --houses gallery on lavapipe (solved, 35f night)

It segfaulted in lavapipe's queue thread: lavapipe maps every allocation
on its own, the gallery peaks near 50 000 mappings, past the kernel's
default vm.max_map_count (65 530) when the near shadow's moving layer
(the spinning sails and wheel) adds its share; lavapipe's mmap fails and
it memsets MAP_FAILED. Not the game's code. `sysctl -w
vm.max_map_count=262144` fixes it (root works in the container; run_test.sh
and run_fast.sh do it). A real GPU driver suballocates: nothing to fix
there. Found with gdb (memset's destination -1 after a pipe_screen map
call) and /proc/PID/maps counts.

## Generating buildings

Radu's rule: check buildings in house_view (seconds), not in the game
(minutes). The game's `--houses` gallery works on lavapipe once
vm.max_map_count is raised (above).

```
./tools/potop_house --recipe house_plans/NAME.house objects/NAME.pobj
./tools/house_view --recipe house_plans/NAME.house --out /tmp/x/NAME --views front,close,above --size 900x600
```

- The output path is relative to data/ (objects/NAME.pobj lands in
  data/objects/; data/objects/NAME.pobj would land in data/data/objects/,
  which happened in step 34y). An absolute path is taken as is.
- ALWAYS give the output path. Without it the tool writes the recipe's
  `output`, else the preset's name: every workshop and mill went to
  cottage.pobj once, overwriting his cottage. Running potop_house with no
  arguments writes town_house.pobj.
- house_view needs xvfb-run and lavapipe like the game. `--look
  EX,EY,EZ,TX,TY,TZ` places the camera (model meters), `--detail N` draws a
  level of detail, `--collision` the collision shell.
- Checks used before (scripts lost with the container, easy to redo with
  Python and PIL): z-fighting (render twice with depth bias variations and
  diff), holes (sky colored pixels enclosed by the building: flood fill
  from the image border over sky pixels, whatever sky is left is a hole).

## Texture tool

tools/potop_texture makes procedural textures (coursed limestone,
weatherboards, sail canvas...). See its --help. Radu's real textures are in
data/props/textures (limestone, granite, sandstone, field_stone, drystone,
slate, house_*...): prefer his real ones; do not put the procedural
coursed_limestone everywhere (that happened, step 34b undid it).

## The gallery's packing (35r)

engine/potop_app.c `house_gallery_pack`: each type a rectangle from its
model's bounds (turned a half turn as it stands, out to its spinning
parts' reach, the 4 m gap round it), laid bottom left on a skyline 150 m
wide in 0.5 m cells, each where the skyline under it is lowest. It was
rows as deep as their biggest member and each house's bounding circle
round its origin (the stana's origin at its hut, its pen 16 m off: a 33 m
square). 62 houses took 149 m before; 73 take 117 m now. The fast root
(potop_fast) needs new .pobj files linked and new textures shrunk by hand
(its objects.ini is a copy there with the new sections, not the project's).
