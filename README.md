# Potop

*"Potop" means "flood" in Romanian, the biblical kind.*

A game engine for big outdoor worlds, written from scratch in C11 on SDL3:
forests, rivers, villages and mountains several kilometers across, water
that really flows, villagers with lives of their own, and physically based
rendering. It is meant mainly for role playing games and first person
shooters; the engine is a library (`engine/`, its API in `engine/potop.h`)
that a game's own `main` calls.

It draws through SDL_GPU: Vulkan on Linux, Android and Windows; Metal on
Macs once the shaders are translated for it. Linux is where it is made and
tested; Android comes next.

## What it does

**Terrain and water**

* Procedural terrain with soil types over bedrock, terrain brushes and
  stamps (crater, dam, hill, channel, lake, plateau), materials painted by
  slope, height and soil.
* A water simulation on the GPU: springs, rain, a water brush and a sponge;
  rivers and lakes find their own way down, the water erodes the ground,
  carries mud and drops it as silt. Dams break.
* Baked maps: the water frozen where it settled, for game worlds that do
  not need it moving; active maps keep it simulated.
* Maps of 4096 x 4096 cells (4 x 4 km at 1 m a cell) with about a million
  objects.

**Objects and physics**

* Rigid bodies (Box3D): the water floats, pushes, tears loose and carries
  off what stands in it; houses split a flood.
* Physics bodies made around the player, the water and the actors as they
  are needed; the objects in a grid, so the cost per frame follows what is
  near and seen, not the size of the map.
* Vegetation and village layouts (homesteads, hamlets, camps, mills,
  fields) grown on new maps, or planted and placed by hand.

**Rendering**

* Physically based materials, sun shadows (a far map and a near one),
  contact shadows, sky occlusion baked over the map, ambient occlusion and
  bounced light from the screen (GTAO, with its own temporal filter).
* Volumetric clouds and fog, rain, snow, a rainbow, the seasons and a day
  and night cycle with the moon and the stars.
* GPU culling with occlusion, impostors for distant objects, temporal
  anti-aliasing on the buildings, everything drawn relative to the eye (no
  shimmer kilometers from the origin).

**The world's people and the game side**

* Actors with skeletal animation, path finding on a navigation grid built
  where it is needed, and animals that roam by their bodies.
* Villagers with a day of their own on the game clock (the smith works,
  eats at the inn, goes home), simulated even out of sight.
* Weapons with physical projectiles, explosives, an inventory, picking
  things off the map, dialogs, a journal, a compass and a map.

**Tools** (in `tools/`, all in C)

* A tree and plant generator and editor, a prop generator (stones,
  barrels, tools), a procedural PBR texture generator, a medieval building
  generator (from hovels and cottages to inns, smithies, mills and
  churches) and a converter for glTF models and older formats.
* A separate map editor (`editor/`).

## Building

The code is in four places: `engine/` (the engine, built as a library,
`build/libpotop.a`), `game/` (the game: the kit, the hits and the loot,
The Hunt, Moș Martin, and its `main.c`, written against `engine/potop.h`
alone: **`potop`**), `editor/` (the editor: the tools, its windows, the
inspector, the console, the tests: **`potop_editor`**), `tools/` (the
desktop tools). The editor walks on foot too, but plays nothing: no
weapons, no HUD, no quests.

`engine/potop.h` is the engine's interface for a game. `potop_start` makes
the window, reads the settings, makes a world (or loads a map) with the
player on foot and the HUD; then the game's loop is
`potop_frame_begin` (the events: what the player did, as a
`potop_input_t`), the game's own rules, `potop_frame_end` (the HUD, the
walker, the world's steps, the weapon, the picture). In between it asks
and tells: actors, items and containers, the journal, conversations, the
HUD, the player, all by ids (no pointers into the engine), what the engine
says about something as a small struct, a copy. The engine reports, the
game decides. It changes the world by hand too (`potop_edit_*`: raise,
dig, paint, smooth, stamps, water, springs, planting, clearing, objects,
layouts, lights), one stroke at a point, so an editor or a game with a
changing world holds its own brush; the editor's tools (`editor/tools.c`)
are such a brush. Maps are saved and loaded through it (`potop_map_*`).
Every object on the map has an id that stays while it is there and is
kept in the map's file (the editor shows it on Alt): a game names its door
or its chest by it (`potop_object_get`, the player's target's
`object_id`); only the objects the game marks
(`potop_object_set_interactive`, again after each map load) are E's
target, besides what can be picked, lies on the ground, and the actors.
Its insides (`potop_engine.h`, the rest of `engine/`) are open for whoever
needs more, knowing they may change.

The game takes `--map NAME`, `--seed N`, `--width W --height H`; the
editor takes everything below (and runs the tests).

Both have the console (\`, the same commands; a game adds its own with
`potop_console_add_command`) and the settings window (every tab for now;
a row can be the editor's only). In the game Escape with nothing open
opens the settings, until it has a menu of its own.

Needs a C11 compiler, `make`, `pkg-config`, **SDL3** (3.2.30 or newer, 3.4 recommended) and, to
compile the shaders, **glslangValidator** (`glslang-tools` on Debian based
systems). To run: a Vulkan driver (any GPU from about 2012 on; Mesa's
`lavapipe` runs it in software, slowly, for machines without one:
`mesa-vulkan-drivers`).

```sh
make                    # optimized build with debug info, and the shaders
make shaders            # only the shaders: data/shaders/*.vert|frag|comp to data/shaders/spirv/*.spv
make DEBUG=1            # -O0 with address/undefined sanitizers
make OPENMP=0           # without OpenMP (terrain generation single threaded)
make tools              # desktop tools: tools/potop_convert (actors and glTF, see "Actors"),
                        # tools/cloud_bake (clouds grown in a simulated atmosphere)
./potop                 # the game
./potop_editor          # the editor
```

If your distribution has no SDL3 package yet (Ubuntu 24.04, for example), or
an older one, build it from source:

```sh
wget https://github.com/libsdl-org/SDL/releases/download/release-3.4.8/SDL3-3.4.8.tar.gz
tar xf SDL3-3.4.8.tar.gz
cmake -S SDL3-3.4.8 -B SDL3-3.4.8/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL3-3.4.8/build -j && sudo cmake --install SDL3-3.4.8/build && sudo ldconfig
make
./potop_editor
```

Installed in `/usr/local` (the default), pkg-config finds it by itself. To
install it elsewhere without root, add `-DCMAKE_INSTALL_PREFIX=$HOME/sdl3` to
the first cmake line, drop the `sudo`s, and point pkg-config at it:
`PKG_CONFIG_PATH=$HOME/sdl3/lib/pkgconfig make` (in front of `make`, as an
environment variable: passed as a make argument it does not reach
pkg-config). The SDL library path is stored in the executable (rpath), so no
`LD_LIBRARY_PATH` is needed to run it.

Not older than 3.2.30 (the Makefile checks): from 3.2.24 to 3.2.28 SDL's
Vulkan memory defragmentation, which runs once the window has changed size,
marks the buffers it moves as index buffers, and from then on compute passes
read stale data on NVIDIA cards (the water's depths went negative, culled
objects were drawn from stale lists).

SDL builds its Vulkan support without extra headers. `POTOP_GPU_DEBUG=1`
in the environment turns on SDL_GPU's checks (and the Vulkan validation
layers, where installed).

The game looks for `data/` in the working directory first, then next to the
executable.

## Controls

| Input | Action |
|---|---|
| Left mouse | apply the current tool (on the map; clicks on the interface stay there) |
| Shift + left mouse | add water |
| Ctrl + left mouse | sponge (remove water) |
| Alt (held) | what is under the pointer: the nearest object or actor glows, and a list in the bottom left corner (and the log) has everything the ray goes through, nearest first, down to the ground, each with its id (#...: what a game names it by) |
| Right mouse drag | rotate the camera (yaw and pitch) |
| Middle mouse drag | pan |
| Mouse wheel | zoom |
| W A S D / arrows | pan |
| Q / E | rotate |
| 1 2 3 4 | tool: raise, dig, paint soil, smooth |
| 5 6 7 8 | soil for raise / paint / raising stamps: sand, earth, clay, rock |
| T | stamp tool; press again for the next stamp: crater, dam, hill, channel, lake, plateau |
| G | plant tool; press again for the next category: trees, dead wood, bushes, grass, flowers, rocks, cacti, water plants, crops |
| H | clear tool: removes every object under the brush |
| O / Shift+O | place one object of the plant category under the cursor / remove the nearest object |
| K / Shift+K | place an actor under the cursor, facing the camera / remove the nearest actor |
| Ctrl+K | the next actor type K places |
| J / Shift+J | every actor to its next / previous action (debug) |
| F8 | navigation overlay: where the first actor type cannot go and why (red: objects, orange: too little room, yellow: too steep on every side, faint yellow: on some sides, blue: object tops to stand on, violet: water it cannot pass), the actors' paths in green |
| left click on / near an actor | select it (tests); then every left click sends it there, escape lets go |
| V / Shift+V | grow the map's vegetation anew (keeps placed things like houses) / remove all objects |
| L | layout tool; press again for the next layout (garden, homestead, fortune teller, camp, desert camp, hamlet, mill, field, fishing, ruins); click places it |
| Delete | remove the layout nearest the cursor, with everything it placed |
| - = | stamp height (0.5 m steps) |
| [ ] | brush radius |
| F | place or remove a spring (fountain) under the cursor |
| R | rain on / off (it wets the ground; it adds no water) |
| B | map borders: water drains away / walls keep it in |
| U | sky occlusion: worked out anew over the whole map (Settings > Render) |
| M | water model: pipes sized by thin water's depth / classic fixed pipes (for comparing; on foot M is the map) |
| Space / P | pause everything (on foot, Space jumps; P still pauses) |
| F7 | stop / start the water simulation alone: the water stays as it was, drawn flowing, while the physics keeps running (see "Frozen water") |
| C | on foot (first person) / back to the orbit camera; Esc leaves it too |
| 1 2 (on foot) | weapon up / away, the other one (see Weapons); with one up, left mouse fires, right mouse (held) aims, R reloads |
| E (on foot) | what the eye is on: talk, loot a dead animal, pick up what lies there, pick a plant (see Inventory, Dialogs) |
| I (on foot) | the inventory |
| M (on foot) | the world map (in the orbit camera M still switches the water model) |
| J (on foot) | the journal (quest log; in the orbit camera J still cycles the actors' actions) |
| , . | slower / faster simulation (x0.25 to x16) |
| N | new terrain with a random seed |
| F1 | wireframe (where the GPU draws lines) |
| F2 | debug view: soil types, the material map (flat colors), off |
| F4 | log the diagnostics now: simulation (also every 30 s of game time while water is awake) and the render statistics |
| F3 | water tile overlay: red active, orange active with mud, blue sleeping, violet sleeping with mud |
| F6 | thin water highlight (debugging): water up to `thin_water_millimeters` in bright magenta where it moves faster than the sleep speed (it keeps its tile awake), dark purple where it stands, films too thin to flow in cyan |
| F12 | screenshot (PNG in the working directory) |
| F5 / F9 | quick save / quick load (the map named `quicksave`, of the kind the map is: active or baked) |
| F10 | hide / show the interface |
| Esc | close the console, or the topmost window of the interface |
| ` or ~ (the key left of 1) | the console (see below) |
| Tab (on foot) | let the pointer out to use the interface (W A S D still walk) / take it back; a click on the world takes it back too |
| 0 (on foot) | put the tool away: no brush, nothing applied (for looking around, at night above all) / take it back; choosing a tool takes it back too |
| Ctrl+Q | quit |
| F11 | full screen on and off |

On foot (C) the mouse looks around (its field of view, Camera → On foot, from
15 to 110 degrees: 70 is a 17 mm lens on a 35 mm camera, 30 about 45 mm;
Invert mouse in the Camera settings
turns it into a flight stick), W A S D walk, Shift runs, Space jumps. Water
over the knees slows the walk and its current pushes; deeper than the chest
the walker swims at the surface, carried by the current. Swimming forward
goes where it looks: looking down it dives, looking up it comes back to the
surface; Space swims straight up, Ctrl dives straight down (the underwater
view takes over below the surface). Left alone under water it drifts slowly
back up. The tools work
where the walker looks, at the middle of the screen, with the same mouse
buttons and modifiers.

The walker stands on what the navigation knows: the terrain, a bridge's
deck, a big rock's top (it steps up to 0.5 m; higher, it jumps). It is a
capsule 0.3 m around and 1.7 m tall in the physics (Box3D's character
mover): houses, trees, walls, fences and loose things heavier than 2 t (a
boulder) stop it by their real shapes, and it slides along them; what is
lower than its step it steps over. Standing objects (static and anchored
bodies) collide with the model's own triangles, branches and all; one torn
loose falls with the simpler moving shape it had before. Loose things
lighter than 2 t (barrels, logs, boats, stones, benches) do not stop it:
it walks into them and pushes them, a barrel up to its own speed at once,
a log only as hard as a 75 kg walker can. Anchored things it never tears
loose: that is for the water.

Stamps act on a click. Dam and channel are dragged: press at one end, release
at the other. A dam gets a flat, horizontal crest the stamp height above the
higher of its two ends, and is extended into the banks until the ground
reaches the crest, so water cannot simply walk around it. A channel's bottom
slopes evenly from one end to the other. The lake stamp digs a basin and fills
it up to its lowest rim point. The outline shows the stamp's footprint.

The status line at the bottom shows the frame rate, the simulation speed (and
the speed actually reached when the machine cannot keep up), water volume,
suspended mud, awake water tiles, rain, the number of objects and the current
tool; the diagnostics window has the rest. With `--no-ui` the window title
shows all of it instead.

When the window is in the background (no keyboard focus, or minimized), the
simulation pauses (`pause_in_background`) and the game draws only a few
frames per second. If the simulation cannot keep up, it gets at most
`maximum_milliseconds_per_frame` per frame and the game runs slower instead
of falling behind.

Every warning (missing textures, unknown settings, fallbacks, GPU errors) is
printed to the console. While water is awake, every
`diagnostics_interval_seconds` of simulated time a line says what woke tiles
up, what keeps them awake, how much mud is in suspension and what the water
and erosion cost per step, followed by the three fastest active tiles.

## Interface

Built with Nuklear (see Third party). Every key above still works; the mouse
over a window of the interface does not reach the map (no digging through the
panel, the wheel scrolls the window instead of zooming).

What the player sees on foot (the compass, notices, dialog, journal, map,
inventory, loot, prompt, names) is the HUD, engine/hud.c: the game gives it
a hud_view_t each frame and reads back hud_requests_t. The editor's windows
are engine/ui.c; both draw with the same backend, font and colors.

* **Brushes** (top left, always there): the tools by group. Terrain: raise,
  dig, paint, smooth, stamp. Water: pour, sponge, spring, plus the rain and
  border switches. Nature: plant, clear. Villages: place, remove. Debug:
  inspect (see below). Below them
  the brush size and the options of the current tool (soil, stamp type and
  height, plant category, layout, rates). Drag it by its title, fold it with
  the button there. It is see-through until the pointer is over it.
* **Icon bar** (top right): settings, save, load, new map, pause, slower,
  the speed, faster, diagnostics, status line.
* **Settings**: tabs for graphics, clouds, water, new map, objects, camera
  and the interface. Most sliders act at once. Texture size, anisotropy, relief,
  shadow map sizes, vsync (graphics) and the vegetation density wait for
  **Apply**, since they rebuild something. The new map tab shapes the next
  generated map (size and cell size included), with a seed field and
  Generate. **Save** keeps the changed values in `settings_changed.ini` in
  the preference directory (only what differs from `data/settings.ini`);
  **Restore** goes back to `settings.ini`.
* **Diagnostics**: GPU time per pass (reads 0 for now: SDL_GPU has no timer
  queries yet), terrain detail levels and uploads,
  objects drawn, water tiles and what the simulation costs per step, live.
* **Inspector** (the inspect tool, in Debug): everything the simulation knows
  about the cell under the pointer. A click pins the cell (it stays shown
  while the camera moves, marked on the map); a click on it again, or
  **Let go**, frees it. It lists the cell's water (dry, film, thin or deep),
  level, ground, soil, moisture and mud; for each of the four pipes the
  outflow, the inflow from the neighbor, the speed, the level difference and
  which rule decides it (superfluid, shut by the meniscus, shut by static
  friction); the tile's state, what keeps it awake (flow or level change
  against the sleep thresholds) and why it last woke; erosion, soaking and
  drying of the cell; the nearest body and what the water last did to it; a
  5x5 grid of the levels around the cell; and graphs of the cell's level and
  flow over the last ten seconds. **Log** writes it all to the log. The pipe
  rules are computed by the same code the simulation runs, so what it says
  is what happens.
* **Console** (` or ~): across the top, the log (everything the terminal
  gets, warnings in yellow, errors in red) and a line for commands. On foot
  it lets the pointer out while it is open. Enter runs the line, Up and Down
  bring back earlier ones, Tab completes a command or a setting (several
  fitting: the common part, and the list), Page Up / Down and the wheel
  scroll, Ctrl+L clears the log, Ctrl+U the line, Ctrl+V pastes, Escape or
  ` closes. The commands:

  | Command | What it does |
  |---|---|
  | `help [command]` | the commands |
  | `list [part]` | the settings whose name has the part, with their values (`list cloud`) |
  | `get <setting>` | a setting's value: `section.key`, or the key alone where only one section has it |
  | `set <setting> <value>` | changes it, written as in `settings.ini` (`set cloud_coverage 0.7`, `set render.sun_color 255, 200, 150`); a cloud type brings its values, a changed cloud value makes the type custom, as in the settings window. Not saved: Save in the settings window does that |
  | `apply` | what waits for Apply in the render settings (texture sizes, shadow maps) |
  | `where` | where the eye is and its yaw and pitch |
  | `goto <x> <z> [height]` | to a point of the map, in meters (on foot it stands there, or falls from the height) |
  | `look <yaw> [pitch]` | turns the eye, in degrees as `where` gives them |
  | `distance <meters>` | how far the eye is from what it looks at (as the wheel's zoom) |
  | `time [speed]` | how fast the sky's clock goes: the clouds' wind, their forming and fading (1 as the game, 60 a minute a second, 0 still) |
  | `hour [0..24]` | the time of day, or sets it (18.5: half past six in the evening) |
  | `freeze` | stops the day at this hour, or lets it go on (the clouds keep their own clock) |
  | `moon [age]` | the moon's age in days since new (14.75 full), phase and place, or sets it |
  | `lights [on\|off\|auto]` | the lamps (lanterns, candles, placed lights): all lit, all out, or with the day |
  | `paint <coverage\|heaviness\|kind\|flocks> <0..1> [km]` | paints the whole sky (0.5 is what the settings say; flocks: 0 none, 1 everywhere); with km, a brush that far around you adding that much (-1..1) |
  | `maps` | the saved maps, by the names `load` takes, and their kind |
  | `load <name>` | loads a saved map: `name`, `name.active` or `name.baked` (without the kind, the active one if there is one) |
  | `save <name>` / `bake <name>` | saves the map as its own kind / bakes it (for play), as `name.baked` |
  | `screenshot` | as F12 |
  | `clear` / `quit` | clears the log / leaves |

  The same commands are meant to be what the game's API speaks later.
* **World** (top bar): what is shown of the world. Objects: off, they
  are not drawn, nor their shadows and lights; they stay and go on (the
  physics, the water, the paths, saving), only unseen. Fireflies: how many
  on dark nights (0: none).
* **Maps**: save under a name, load or delete from the list. Maps are zip
  archives (`.potop`) in the `saves` folder of the preference directory
  (on Linux `~/.local/share/potop/potop/saves/`). A map keeps the ground
  (heights, bedrock, soils, silt), the water, mud and ground moisture,
  springs, rain and border switches, every object and the camera; the water
  starts still after loading and gets moving within a step. Maps of another
  size than the current one load fine (the world is remade at their size).
  A 4096 map takes about 50 MB and a second or two.

  Two kinds of map (see **Baked maps** below): **active** (to make maps
  with: the water flows, the ground erodes, every tool works) and
  **baked** (for play: still). The save window's **Baked (for play)** box
  picks which; the kind goes into the file name (`NAME.active.potop`,
  `NAME.baked.potop`) and shows in the list beside the name, so a baked
  copy is never taken for the map you work on. A baked map can only be
  saved baked again (it has nothing left to go on from).

`data/ui.ini` sets the font and its size, the size of the whole interface
(`scale`), how see-through the brush panel is, the colors (any of Nuklear's
style colors) and the icons. An icon is a PNG named per button (the list is
in `ui.ini`, the files go in `data/ui/icons/`); a button without one shows
its name. With `icon_labels = true` the tool buttons show the name next to the
icon, otherwise only in the tooltip. Where the windows are,
which are open and the interface tab's values are kept in `ui_state.ini` in
the preference directory; delete it to start over from `ui.ini`.

## Command line

The editor's (`potop_editor`), mostly for automated tests:

```
--seed N                      terrain seed
--load NAME                   start with a saved map (NAME, NAME.active or NAME.baked)
--save NAME / --bake NAME     save the map (its own kind) / bake it (NAME.baked) just before quitting
--size N                      terrain size in cells (multiple of 64)
--width N --height N          window size
--yaw D --pitch D --distance M   initial camera
--first-person                start in first person
--go-to X,Y,Z                 put the camera (first person: the eye) there, in meters;
                              Y '-' stands on the ground
--look-at X,Y,Z               turn the camera towards that point
--heading YAW,PITCH           turn the camera: degrees, pitch up positive
--debug-view N                0 normal, 1 soil types, 2 the material map
--brush-test                  raise a sand hill at the map center (tests the edit/upload path)
--flood-test                  pour water at the map center during the first 4 seconds
--water-test                  water mass conservation test, no window; exit code 0 = passed
--scene dam                   start in the dam scene: a sand dam with a notch, a full lake, a spring
--dam-test [--dam-seconds S]  run the dam scene without a window for S seconds (default 180):
                              the dam must wash out, water and material must balance exactly
--pour-test S [--no-erosion]  no window: pour water (the water brush) at the map center for 15 s,
                              then simulate S seconds, logging diagnostics every 30 s
--fixed-pipes                 start with the classic fixed pipes instead of depth scaled ones
--tile-overlay                start with the water tile overlay on (F3)
--walk-test [DX]              with --obstacle and --warmup: on foot, 6 m before the obstacle (DX
                              to its side), walking into it; where it and the nearest body are, logged
--warmup S                    simulate S seconds before the first frame (actors sent with
                              --actor-goal move meanwhile)
--density X                   vegetation density scale (overrides [objects] density_scale)
--gallery [CATEGORY]          every object type (or those of one category) in rows on a flat
                              floor where the camera looks, nothing else (to look at the models)
--actor-test [ACTION[:S]]     every actor type side by side where the camera looks, side on,
                              optionally playing ACTION, S seconds into it (with --gallery none
                              for a flat floor)
--bridge-test                 with --gallery none --actor-test: a trench 3 m deep across the
                              view, bridged, and the actors 10 m to its side
--navigation-overlay          start with the navigation overlay on (F8)
--actor-goal DX,DZ            with --actor-test: send them DX, DZ meters from where the camera
                              looks; logs the path; with --screenshot a thirtieth of a second
                              per frame (--frames sets how long they walk)
--ramp DEGREES                with --gallery: the floor slopes up along +X (actors on slopes)
--plant-test                  plant every category with the brush, in circles around the target
--layout NAME                 place a layout where the camera looks (after everything else)
--screenshot FILE [--frames N]   render N frames (default 3), save, quit
--no-ui                       no interface: keys only, statistics in the window title
--load NAME / --save NAME     start with a saved map / save the map as NAME before quitting
                              (--save works with --screenshot)
--open WINDOW                 open a window of the interface at start: settings (settings:N
                              for tab N), diagnostics, save, load, new, sky, console
--console "A; B"              run console commands at start, one after the other
                              (--console "time 60; goto 500 800; look 90 10")
--tool N                      start with tool N (the order in tools.h; no tool by default)
--sky-test                    paint the sky clear but for heavy clouds over the map
--thin-highlight              start with thin water highlighted (F6)
--pick                        as if Alt were held, along the ray through the middle of the screen
                              (the list goes to the log)
--effect NAME                 a particle effect (data/particles.ini) on the --obstacle, or at
                              the camera's target; with --warmup it plays that long first
--inspect                     pin the inspector on the cell the camera looks at; with --screenshot
                              its report is logged at the end
--physics-test                with --scene dam: logs, barrels, a crate, a haystack and a boat in
                              the lake, trees, houses, fences and grass below the dam
--map-test                    save a flooded map with a layout and a spring, load another map
                              of another size, load the saved one back and compare everything;
                              exit code 0 = passed
--ui-test                     made-up mouse events: a click on the panel must pick the tool and
                              not reach the map, the wheel over it must not zoom, a click on
                              the map must reach it; then Apply's rebuilds; exit code 0 = passed
```

The tests take a few seconds on small maps (`--water-test --size 256`); on
big maps the rain settle part of `--water-test` takes minutes.

## Tuning

* `data/settings.ini`: terrain generation, terrain cache, camera, rendering,
  shadows, water look and physics, tools, simulation rate, objects (populate
  new maps, density and draw distance scales), object physics (`[physics]`). Removing a key (or
  the whole file) restores the default compiled into `engine/settings.c`; the
  meaning of every value is commented in `engine/settings.h`.
* `data/soils.ini`: what each soil does (sand, earth, clay, rock and silt, the
  mud the water leaves behind): how fast it gets wet and how much water it
  holds, bed friction, the flow speed it starts eroding at, how fast it
  erodes, and the steepest slope it holds under water. Also transport and
  deposition rates and the generated soil depth.
* `data/objects.ini`: object types (see Objects below): what each model is,
  how big, how far it is drawn, its shadows, its physics (body, anchor,
  floating, flood behavior, damage), where it grows and how densely.
* `data/layouts.ini`: layouts (see Layouts below): which objects make a
  homestead, a camp, a hamlet, and where new maps get them.
* `data/materials.ini`: terrain textures. For a texture named `grass_01` the
  game loads `data/textures/grass_01.png` and, if present, its normal map
  `data/textures/grass_01_normal.png` (green up the picture, OpenGL's;
  `normal_green_down = 1` for DirectX's); without one the normals come from
  `data/textures/grass_01_height.png` (the color + height pair texgen
  exports), or else from the brightness. Up to three variants per
  material. Where two materials meet each breaks into patches of the other,
  not a blend by height: `patch_meters` (how big its patches), `patch_edge`
  (how soft their edges, meters; 0.03, at least a pixel and a half: far off
  it turns into a plain blend), `patch_strength` (how far they break the
  border; 0: a plain blend). The noise is in the shader, in world space:
  nothing repeats, and only the materials that show in a pixel are read.
  `wet_texture`: what it looks like soaked (clay: `wet_texture = mud_01`,
  made by `potop_texture mud` with the clay's seed). Where the ground is
  under water (at once) or soaked (clay soaks slowly) it shows that
  instead, in patches as it dries, the edges first: the clay's cracks close
  under the water and open again as it dries. The silt the water leaves can
  have one too (`[silt] wet_texture = mud_01`). Roughness (how matte, 0 a
  mirror .. 1 chalk): `grass_01_roughness.png` (gray; or the green of
  `grass_01_arm.png`), else the material's `roughness` all over (0.9; rock
  0.75, snow 0.6), its wet texture's `wet_roughness` (0.35). The sun's
  highlight follows it (GGX), and smooth ground (roughness under 0.5: wet,
  under water) mirrors the sky a little at grazing angles; wet ground is smoother still (the water in its pores), and
  under standing water nearly a mirror. The layers are made once (decoded,
  normals and roughness put in, mipmapped) and kept compressed (BC7) in
  `data/cache/textures`, as are the models' textures: next time they go up
  as they are. A CRC32 of everything they were made from (materials.ini,
  every source file's bytes, `texture_size`, `normal_strength`) decides
  whether they are made again; a texture with a `_normal` file has its
  `_height` file left unread. Delete the folder to make them all again.
  `tiling_meters` is the size of one repeat up close,
  `macro_tiling_meters` the size at a distance (the shader fades from one to
  the other; if omitted it is ten times `tiling_meters`). Without any texture files every material uses a procedural
  fallback, so the game runs with an empty `textures/` folder. Silt has its
  own material, `[silt]`.
* `data/shaders/`: GLSL, compiled to SPIR-V by `make shaders`
  (`data/shaders/spirv/`), loaded at startup: after editing one, `make
  shaders` is enough, the game itself need not be rebuilt.

For texgen textures, export with `light_strength = 0`: the game lights the
relief itself from the normals, and baked lighting would fight the real sun.

The look of the ground is tuned from `[render]` in `settings.ini`: blend
width between soil types (`soil_blend_meters`), slope ranges for grass and
bare rock, a global variation texture over all materials (load your own as
`data/textures/variation.png`, grayscale and tileable, or let the game
generate one) and moving cloud shadows (coverage, wind, height). The same
variation texture is used for the clouds.

Rendering speed, also in `[render]`:

* `lod_distance_meters`: terrain level of detail. Chunks farther than this
  are drawn with every second vertex, twice as far with every fourth, and so
  on (down to every 32nd). Vertices slide onto the coarser surface before a
  chunk switches, so nothing pops, and lighting uses per pixel normals, so
  far terrain keeps its relief detail. Raise it if distant ridges look too
  smooth; 0 draws everything at full detail. It is never set lower than 2.5
  chunk diagonals (about 57 m at 0.25 m cells), below which seams could open.
* `far_terrain_upload_seconds`: ground that changed beyond full detail or out
  of view (erosion under far water changes it every step) is sent to the GPU
  at most this often; up close and in view it goes at once, and a chunk that
  comes close or into view is sent right away. 0 sends everything at once.
* `far_water_upload_seconds`: the same for the water (0.2 s). Moving water
  is sent to the GPU every step otherwise, 12 bytes a cell: the depth as a
  full float (the surface is ground + depth, and half floats would put
  centimeter steps on deep lakes), velocity, mud and the F6 speed as half
  floats.
* `render_scale`: the 3D scene is drawn at this fraction of the window
  resolution and scaled up. 0.75 shades about half as many pixels; on a big
  screen that is most of the terrain's cost.

**Day and night** (Graphics → Day and night, `day_cycle`, on unless set).
The sun crosses the sky once a day (`day_length_minutes`, 240 real
minutes; 0 keeps the hour). It is up `daylight_hours` of the 24 (14, about
noon: up at 5:00, down at 19:00): it rises a quarter turn before its noon
direction, stands at Sun height at noon and sets a quarter turn after it.
An hour of dusk takes it 12 degrees under the horizon, the night (8 hours)
as deep as its noon height, an hour of dawn brings it back (the sun
settings are noon's). Low, its light turns
orange and fades; at dawn and dusk the sky turns a dusky blue, with a warm
glow low on the side where the sun rises or sets (only there, fading round
to the other side and upwards), then goes dark blue; the stars come out (a few in a hundred tiny cells of the sky,
most faint, turning about the pole once a day, twinkling, hidden by the
clouds, the volumetric ones too, looked up in their dome) and the moon
lights the land faint and blue and casts the shadows (the light swaps from
the sun to the moon while both are too faint to see).

**The moon** goes round its phases in `moon_cycle_days` (29.5 days of the
game, as the real one; every map starts a day before full moon). It keeps
the sun's path, as far behind it as its phase: new, it goes with the sun
(up by day, unseen); at first quarter it is up from noon to midnight (in
the afternoon sky too, pale); full, all night; at last quarter from
midnight to noon. Its light goes with how much of it is lit (squared: a
quarter moon gives a quarter of a full one's) and with its height; when
it is down, or new, the stars give a little light from overhead, and the
night is dark but for the fires and the lanterns. On the sky it is a ball
`moon_size_degrees` wide (1.5; the real one is half a degree) with the
real moon's map on it (`data/textures/moon.png`: NASA's LRO color in rgb,
its heights in alpha, 512 x 256, public domain), lit from the side its
phase says (Lommel-Seeliger, as the dust lights: the full moon a flat
disc), the craters along the line between day and night shaded from the
heights, the dark part faintly lit by the earth near new moon; behind the
clouds, reddened low down, and the stars do not shine through it.
`moon` in the console tells its age, phase and place, `moon 0` makes it
new, `moon 14.75` full. The water mirrors it too (a plain gray ball there: the ripples
smear it anyway), where the mirror finds no land on the screen.

**Valley fog** (Sky & light, Valley fog; `engine/fog.c`): low milky fog in
the valleys and over the water, its top soft and torn into wisps that
drift slowly with the wind; it forms in the evening (`fog_form_hour`, 19,
thick four hours later), lies through the night and the dawn, and lifts
in the morning (gone by `fog_clear_hour`, 10; `fog_always` for looking at
it). Where it lies: a valley map made from the terrain every few seconds
(each place against the ground smoothed over `fog_valley_reach_meters`,
250: the deeper it lies below, the higher its fog, up to
`fog_valley_height_meters`, 30; over water two thirds of that), its top
smoothed to lie level as a lake's. `fog_mode`: 2 volumetric (the view cut
into froxels, `fog_quality` 0..2: 96 x 54 x 64 to 240 x 135 x 128 cells, each frame a little elsewhere in them and built on the frames before, the same places in the world, the
fog and its light in each, the sun's through the shadow maps, so ridges
and trees throw their shadows into it, and the sky's; gathered from the
eye outwards; `fog_inject.comp`, `fog_integrate.comp`, `fog_apply.frag`),
1 simple (each pixel marches its ray in 16 steps, no shadows:
`fog_simple.frag`, for weaker GPUs), 0 off. `fog_valley_density` (0.08 a
meter), `fog_softness_meters` (its top), `fog_glow` (brighter towards the
sun, 0..0.9), `fog_night_brightness` (0.3: by night its light is that
much of its full; lit evenly from every side it would outshine the dark
ground round it) and `fog_distance_meters` (600) do the rest. Drawn once the
water is in; the particles and the clouds come over it.

**Fireflies** (World window, `fireflies`, 0.5): on dark nights, not in the
rain, swarms of them about the eye (on foot) or where the camera looks:
on earth or clay (where grass grows), not on rock, sand or water, most of
all within a few meters of water. Each swarm (`engine/fireflies.c`) is the
`firefly` effect of `particles.ini` going on for half a minute or so, then
another somewhere else; the fireflies are small and drift a long while,
flashing in bursts all together, as the synchronous kinds do (`flash` in
the effect: six flashes half a second apart, then dark, every 7 seconds;
most kinds flash each on its own: `flash = ..., 0`). At dawn
they go, one swarm after another. The clouds take the light of the hour; at night their light from
the sky drops to a fifth, so they stand dark against the stars. At night the
exposure is set 2.2 stops darker, so the night stays a night. The shadows
are drawn again each time the light has moved a fifth of a degree. In the console, `hour` tells the time and `hour
18.5` sets it (every map starts at 10:00; the hour is not saved), `freeze`
stops the day and lets it go again. The Sky window has the same at its
top: an Hour slider and Stop. See `engine/daylight.c`.

The scene is drawn as light, not as a picture: linear, in half floats,
where the sun glinting on the water may be fifty times brighter than white.
The frame's last pass (`engine/post.c`, Graphics → Light (HDR), `hdr`, on
unless set) makes the picture of it:

* **Exposure.** The scene averaged down to a few dozen texels, the mean of
  the logarithm of its brightness (the middle of the screen counting more),
  brought to middle gray, the way the eye adjusts. It follows slowly
  (`exposure_adapt_seconds`, 1.5 s): from a dark valley up into the sun the
  picture is too bright for a moment, then settles. `exposure_ev` moves it
  by stops; with `auto_exposure` off, it is the exposure alone.
* **Bloom.** The scene halved six times with Jimenez's thirteen tap filter
  (the first time weighing bright groups down, so one burning pixel does
  not blink), then summed back up with a tent at each size: the picture
  blurred at every size at once, a little of it (`bloom_strength`, 0.05)
  mixed into the scene. Everything glows a bit; what is far brighter than
  white glows a lot.
* **Tone mapping** (`tone_mapper`): any light into what the screen shows,
  without cutting it at white. AgX (0) turns bright colors towards white
  instead of flat yellow or cyan; AgX punchy (1, the default) is the same
  with more contrast and color; ACES (2) is the film look, saturated;
  Reinhard (3) the plainest.
* **Night vision** (`night_vision`, 0.7; `night_vision_luminance`, 0.05):
  where the scene's own light (before the exposure) is under that, colors
  fade towards a pale blue, all the way at a tenth of it, as the eye's rods
  take over from its cones (the Purkinje shift). A moonlit field turns
  blue-grey; the ground by a fire keeps its colors.

**Local lights** (Graphics → Local lights, `local_lights`, on;
`local_light_intensity` scales them all): fires, torches, lanterns and
candles light what is around them. A particle effect gives light with
`light = r, g, b` in `particles.ini` (the campfire, sparks, the magic
ones): it comes up as the effect starts and a burst's dies with its
particles. An object type does with the same keys in `objects.ini` (the
torch, the lanterns, the candles), from near the top of its model unless
`light_height` says where. `light_intensity` (1: about the noon sun on what
is a meter away), `light_radius` (meters; nothing beyond), `light_flicker`
(0 steady .. 1 a dancing flame: its brightness and, a little, where it is).
They light the ground, the objects, the actors and the water; none casts a
shadow. By day the sun drowns them: they fade out as it rises above the
horizon (gone, and costing nothing, from 8 degrees up) and in as it sets;
flames and cores still show. Lamps (`light_switched`, true for objects
unless set: lanterns, candles; the torch burns on) are lit at dusk and put
out at dawn, each at its own moment (`lamps_with_the_day`, on), or all at
once by the console (`lights on|off|auto`) or the game (`lights_set_lamps`).
`light_core` (meters) puts a small glow where the light is, a candle's
flame or a lantern's glass, drawn facing the eye and halo'd by the bloom:
no glowing textures needed, the lantern's frame stays dark.
`light_height` below 0 is that share of the model's height (-0.6, the
default: about a lantern's glass). The **Light** tool (Debug) places lights
(lamp, fire, cold, white; brightness, reach and height) and a click next to
one removes it; they are kept with the map (and the continuous effects
the Effect tool plays, where they are; on an actor they are not). `engine/lights.c` gathers each frame the 512 nearest to the eye (out
to 300 m) and sorts them into clusters: the screen in 16 x 9 tiles, each
cut in 24 slices by the distance from the eye (closer slices thinner); a
pixel (`data/shaders/lights.glsl`) looks only at the lights of its own
cluster, so hundreds cost about what a few do.

Then gamma, and half a step of 8 bit noise against bands in the sky.
Off, the light is cut at white, as the game used to show it (but the water
and the clouds now blend in linear light). The pass also stretches the
scene over the window (`render_scale`).

F4 logs how long the GPU spends per frame on the shadow map, terrain, water
and the upscaling, and how many terrain chunks were drawn at each level of
detail.

Generated terrains are cached in the user's preference directory
(`~/.local/share/potop/potop/cache/` on Linux) and reused when the same seed
and settings come up again; only the newest few files are kept.

## Objects

3D objects are Eternal Lands `.e3d` models, kept in `data/objects/` with
their textures next to them (the names inside the model must match, in any
case). DDS textures are decoded to plain RGBA at load time, because most
phone GPUs cannot read the DXT compression; textures are shared between the
models that use them. Cut out textures (leaves, reeds) get the colors of
their opaque pixels bled into the transparent ones before mipmapping, so
foliage does not grow dark fringes in the distance.

Every object is an instance of a type from `data/objects.ini`: a model plus
its size range, draw distance, shadows, footprint, how it takes part in the
physics (see Physics below) and where it grows. The keys are explained at the top of the
file; a `[defaults]` section sets values for the types after it.

New maps are populated when they are generated (and with V): for every type,
in the order of the file, a jittered grid of candidates is laid over the
map, each kept with a probability from the type's number per hectare and
from the spot: soil, slope, height, standing water, patches (all trees share
one forest noise field, grass grows outside it, flowers in their own small
patches) and valleys (tall grass, reeds and pebbles seek the low ground, big
rocks the ridges). Solid things keep a footprint free, so trees do not grow
through rocks and grass does not grow out of trunks. The result depends only
on the terrain's seed. A 1 km map gets about 250 000 objects (mostly grass)
in under half a second.

The plant brush (G) plants one category at a time inside the brush circle,
up to the density the types have inside their patches; it keeps footprints,
slopes and water rules, but leaves the soil and the place to the player.
Lily pads only grow on water: flood a pond, press V. They float on the
surface and follow it.

Objects stand on the lowest ground under their trunk and follow it when the
terrain changes. They are sorted into a grid of 32 m cells, and within a
cell by draw distance, so a frame only looks at what can be seen: grass
stops at 70 m, trees go on to the horizon. Over the last fifth of its draw
distance an object fades out with an ordered dither (no blending needed);
`[objects] draw_distance_scale` in settings.ini scales all draw distances.

Objects are drawn GPU driven: every object has a fixed slot on the GPU
(position, size, rotation, model), and only the slots of objects that
changed go up (moved by the physics, laid down by the water, put back on
the ground, added, removed). Each pass (camera, far and near shadow map) the
CPU only marks the grid cells in view; a compute shader
(`objects_cull.comp`) tests every object in them (draw distance, its box
against the frustum), writes the visible ones grouped by model into the
pass's instance buffer, and fills the indirect draw arguments: each
material of each model is a single indirect draw call however many objects
use it. On a 4 km map the slots take about 128 MB of video memory. The
camera's pass also leaves out what is hidden: before the water, the scene's
pass pauses and folds its depth (terrain, objects) into a pyramid (Hi-Z,
`hiz.c`: each level the farthest depth of the four texels under it), and
the next frame's culling, before the scene's pass, leaves out an object
whose box is behind that depth over its whole outline on screen (a
mountain or a house between it and the camera), tested with the camera
matrix the pyramid was made with. Being a frame old, something that moves
can hide what was behind it for a frame; what turns into view is outside
the old pyramid, so it is drawn. (On SDL older than 3.2.30, NVIDIA cards
drew objects from stale culling, "ghosts" with another's place and size,
once the window had changed size: see "Building". Two sets of instances
swapped each frame and a widened view worked around it; gone since.)
The pyramid's level 0 is made by a fragment shader
and the rest in a storage buffer that the culling reads, never by a
compute shader sampling a texture written in the same frame: SDL_GPU's
Vulkan backend does not order such reads after the write (its barrier
into the sampled state covers only the vertex and fragment shaders; the
sync validation layer flags it), and the culling, reading a half made
pyramid, hid or showed objects wrongly for a frame, more often at high
resolutions (full screen). A count and a write that decide twice could
also disagree (see `objects_cull.comp`): the write now takes the count's
decision. They cast
shadows (leaves and twigs through their alpha, so the shadows have holes):
big things into both shadow maps, small ones (grass, flowers) only into the
near one, lily pads none.

Far away, the models worth it (150 triangles or more, at least 1 m across
their bounding sphere's radius) are drawn as impostors: pictures of
themselves (`impostors.c`). Each such model gets an atlas of 8 x 8
pictures, 64 pixels each, taken from all round and from above (the upper
hemisphere unfolded on a square, hemi-octahedral), of its color and of its
normals and depth. An impostor is a square facing the eye, showing the
three pictures taken from nearest where the eye is, blended; each picture
is read where the eye's ray through the square meets that picture's own
plane, so the three line up. It is lit like the model (sun, shadow maps,
clouds' shadows, the sky and the ground, local lights, haze), its depth
pushed back or forth by the pictures' depth so it meets the ground where
the model would. The culling picks it once the object is smaller on screen
than `[objects] impostor_pixels` (its sphere's diameter, 64 pixels by
default: Pictures from, in the settings; the projection's focal length,
the scene's height and the size decide, so at a higher resolution they
switch later). Up to 64 pixels (a picture's size) it shows no less than
the model would; above that the picture is seen enlarged, a little soft; what tumbles (loose, laid down) stays a model. The shadow maps
keep drawing the models. `[objects] impostors = false` in settings.ini (Far
objects as pictures, in the settings) turns them off, `impostor = false`
on a type in objects.ini keeps that type whole, and the console's `objects
impostors all` draws every object that has one as its impostor, near too,
to look at them (`auto` puts them back far away); `objects impostors show`
(Show impostors, in the World window) tints them magenta.

The atlases are baked on the GPU when the game starts (or a model or one
of its textures changes on disk) and kept in `data/cache/impostors/`,
compressed, with a key made of the model's and its textures' files and
when they were written, and of how the pictures are made: only what
changed is baked again; the rest loads from the cache (about 2.8 MB of
video memory per model). `POTOP_IMPOSTOR_DUMP=<directory>` saves every
baked atlas there as pictures.

Plants move in the wind (Wind in the plants, in the settings:
`render.plant_wind_strength`, `plant_wind_direction_degrees`,
`plant_wind_gust_meters`; not the clouds' wind). All of it is in the
objects' vertex shader, nothing on the CPU: a plant leans with the wind,
more the higher up (with the square of the height, so its foot stays),
the lean swelling in gusts that roll downwind as waves, so a forest bends
tree after tree, and swinging a little on its own. It bends only as far as
the wind is over its stiffness, with the square of what is over it (the
push grows with the wind's speed squared): a bush stands still in a breeze
and moves in a storm's gusts, grass moves in any wind. Its leaves (cut out
materials) shake fast besides whatever the stiffness, each card as one,
from its foot (still, on its branch) to its tip. How much: a plant's pobj
carries its own (tools/plant_editor's View: sway, flutter, stiffness, by
its kind and height unless given; recipe keys `sway`, `flutter`,
`stiffness`): `sway` meters its top leans in a gale (at size 1),
`flutter` meters its leaves' tips shake, `stiffness` 0..1. A type's own
`sway`, `flutter`, `stiffness` in objects.ini (in its section, not
`[defaults]`) win over its model's; a model without its own (made before)
takes its type's, `[defaults]`' included, as before; each `[defaults]`
starts still. The physics, the shadows and the impostors keep them still.

Shadows come from two maps: a far one over the whole map, redrawn only when
the terrain or the objects change, and a near one over a square around the
point the camera looks at (`near_shadow_meters`, 100 m by default, at
`near_shadow_map_size` texels: about 2.5 cm each), fine enough for branches.
The near one is redrawn when the camera has moved a sixth of its width; its
shadows fade into the far map's towards its edges.

Levels of detail: the trees `potop_plant` makes now carry
them (see "Plants"), and the culling draws such a model coarser the
smaller it is on screen: its height under 300, 120 and 50 pixels
(`[objects] lod_distance_scale`, Branches as far as, moves
these: 2 keeps the branches twice as far) drops first its finest branches
(those holding the twigs or the leaves), then the next ones, then all but
the trunk; the foliage stays (thinned out at the last two levels to
`lod_foliage_kept` of it, 1 by default: all). It costs no more draws than
there are levels: each level of each model is one more indirect draw of
the start of the same index buffer. Nearing the next level, what it leaves
out thins out in a dither over the last 15% of the way, so the branches
do not pop away (the bark's vertices carry their branch's level, in the
tangent's size). The shadow maps draw the trees at least a level coarser
(the near one) or two (the far one): the finest branches' shadows are lost
in the leaves'. `[objects] lod = false` (Fewer branches far away) draws
every tree whole. Older trees and every other model have no levels: drawn
whole, as before.

Grass (`grass.c`, `grass.comp`, `grass.vert`, `grass.frag`): blades made
on the GPU every frame around the eye, nothing stored, on the ground the
terrain shows as grass (worked out as `terrain.frag` does: soil, slope,
sun, silt, bare rock, cliffs, snow). A compute shader looks at the spots of
a grid fixed to the world (so the blades stay put when the eye moves),
fine near the eye and coarser ring after ring farther out (each twice as
far, looking at every second spot of the ring inside it), the blades
thinning with the square of the distance and growing wider (each blade has
a threshold of its own and shrinks into the ground as the share kept there
nears it: they go and come back one by one, never a ring at once; the
blades of the coarser grids have the lower thresholds, so a ring's edge
changes nothing), so about as many show on the screen near or far
and the work grows only with the logarithm of the distance; they sink
into the ground over the last fifth of the distance. It keeps those in
view and writes them, with the indirect draw's arguments; the vertex
shader makes each blade from nothing, a strip of seven vertices up a bowed
curve: leaning a little its own way, with the wind (the plants' wind and
gusts, the trees' own, plus a sway of its own) and laid down the way the
water flows over it (the more, the faster and deeper; drowned deeper than
itself by 40 cm, it is gone), staying down and muddy in the silt the water
left. Its color is the ground's grass texture under it, with the terrain's
variation, darker at its foot and wet, a little yellow at the tip, some
blades dry. It is lit as the terrain is, the sun shining through it from
behind; it takes the shadows and casts none. The settings (Grass, in the
objects' tab; `[objects]` in settings.ini): `grass` (on), `grass_distance_meters`
(50: how far from the eye), `grass_density` (550 blades a square meter near
the eye; below some 300 it looks thin), `grass_height_meters` (0.3), `grass_width_meters` (0.018),
`grass_tint` (multiplies the ground's color) and `grass_dry_share` (0.12).
The grass objects (the `tuft`s of objects.ini) are still planted as they
were: drop their `spawn_per_hectare` to leave the meadows to the blades.

The plants (`potop_plant`'s, `.pobj` version 3 with `OBJECT_FILE_PLANT`)
have shaders of their own (`plant.vert`, `plant.frag`, `plant_shadow.vert`,
`plant.glsl`), without the physically based shading (a plant has no use
for it, and its foliage is most of what is drawn), and no impostors (their
levels of detail do better far away). Each vertex knows what it is on the
plant (along its branch or card, how high on the plant, a number of its
card's or branch's own, how far out in the crown or how thick the branch)
and each material how it goes through the seasons. The bark takes its
normal map, if it has one, and grows moss on the side facing the north
(away from the noon sun), the more the thicker the branch and the lower,
in patches (`moss` in the recipe; none on bushes and the small plants).
The foliage is lit from either side, the sun shining through it from
behind, darker deep in the crown; each card a little lighter or darker
than the next, its tip lighter (the new growth, `tips`: the conifers'
most: a light yellow green at the sprays' ends). The year goes round
by months (`[render] month`, Month in the World window: 1 the start of
January .. 12.99 the end of December; `set render.month 10.5` in the
console). What the foliage looks like each month is in its pictures: a
picture's months are pictures of their own beside it (`twig_m3.png`,
`twig_m12.png`, `twig_m1.png`: green from March, a little snow from
December, more from January), each month showing its own; one without
shows the picture itself if that is a plain one (`twig.png`, the rest of
the year), else (`twig_m3.png`) the latest one before it (round the year); each of those pictures has a cell of its own
in the plant's atlas, the plant knows which of its cells shows which in
each month (the `.pobj`'s month cells), and the switch is at once, the
whole tree.
The autumn is the shaders': through a plant's autumn months
(`autumn_months` in the recipe, the kind's: October and November) it
comes to each tree a little earlier or later (its place decides) and
within it to each card in its own order, the outside of the crown and the
top first: a card turns to its species' autumn colors (`autumn_colors`,
two, a card somewhere between; `autumn`: how much, 0 for the conifers)
keeping the picture's light and dark, a little muted (ochre and rust
rather than flame red), and where the foliage falls (`falls`) the cards go
one by one past the middle of it (their shadows too), until the tree is
bare; it stays bare until its leaves' month (`leaves_month`, the kind's:
April). The winter's snow (`[render] snow_first_month`, `snow_last_month`:
December to February, Snow from and to in the World window) lies on the
upper sides of the bark, in patches, coming in its first month and going
in the last half month, and the branches sag under it (`snow_sag`: of
the height, at the crown's edge, the more the farther out; the trunk
stays). The foliage's lighter tips, its new growth, only from April to
September (`[render] tips_first_month`, `tips_last_month`; New growth from
and to in the World window): growing through April, going in the last
half of September. The plant editor shows it all (Seasons, and View's Month).

## Actors

Actors are animated creatures (a bear and a falcon for now; people, ducks
and the rest later). Their types are in `data/actors.ini`, one section
each, naming a `.pact` model in `data/actors/`. K puts one under the
cursor (Ctrl+K picks the type), where it plays its idle action; J and
Shift+J switch every actor to its model's next or previous action (a debug
key, to see them all). A left click on an actor (on it, a bird up in the
air too, or within 2 m of where the click meets the ground) selects it,
with a circle around it; from then on each left click sends it there (the
tools do not get those clicks), until escape lets go of it.

Sent somewhere, an actor finds its way (`engine/navigation.c`) over a grid
of 1 m cells. A cell can have more than one surface to stand on: the
terrain, and the tops of big objects, a bridge's deck, a dock, a flat rock
(up to three of those per cell), each with the free room above it up to
the next solid thing. They come from the objects' collision triangles (the
physics' shapes), placed, turned and scaled, sampled every eighth of a
meter: triangles facing up (less steep than about 50 degrees) are
surfaces, the rest only take room. A surface also keeps its height per
quarter meter of its cell, for feet: beside a rock that takes part of a
cell is the ground, not the rock's top. A top covering less than a quarter
of its cell (a pointed rock's tip) is no surface; the cell is in the way. Only the lowest 4 m of an object count, so a tree's
trunk is in the way and its crown is not; objects smaller than
`navigation_object_meters` across ([simulation], 0.3 m) are left out, so
actors walk through pebbles. A wall takes exactly the room it
has, a bridge can be crossed on top and, if there is room, under.

A* runs over the surfaces in 8 directions, never cutting the corner of a
blocked cell. An actor can be on a surface with at least its
`height_meters` of room (by default its model's height), not in water too
deep or too fast for it; it gets from one cell to the next if the height
between their surfaces is within its `step_meters` or its
`maximum_slope_degrees`. Steep steps and water cost more, so it takes a
detour rather than the hill or the lake when the detour is not too long.
The estimate of the way left is weighed a little over the truth (by 1.3),
which keeps the search heading for the goal instead of flooding every
cheaper corner around a hill, for paths at most that much longer than the
best. The search gives up after `path_search_cells` surfaces ([simulation],
100000) and then heads for the reached cell nearest the goal. On rough
ground a search costs about half a microsecond per surface looked at (a
70 m trip through mountains: 30 ms; across a meadow: well under one). The path is pulled tight into at most 32
waypoints (straight lines wherever the ground along them is no worse); a
longer path is searched on from its last waypoint. A straight stretch
also keeps half the actor's `width_meters` (actors.ini; by default its
model's width, 0.74 m for the bear) from anything in the way on either
side, so it does not scrape its flank along a stone or a trunk; the search
itself goes through cell centers, half a meter from their neighbors.

On the way it turns towards each waypoint (`turn_degrees_per_second`),
walks slower uphill and through water deeper than `wade_depth_meters`,
and swims where the water is deeper than `swim_depth_meters`: afloat, its
body that deep under the surface, level, at `swim_meters_per_second`, with
the walk animation (there is no swimming one). The walk plays at the pace
of its speed (`walk_cycle_meters`, the ground one loop of it covers; 0
plays it as it is at walking speed). Running water carries it, wading a
little, swimming fully, moving or not; a current faster than it swims is
a wall for its path.

The map changes under its feet, so it keeps an eye on the path: twice a
second it looks at the stretch it can see (`vision_meters`). The map is
split into regions of 128 m, built again when their terrain moved by more
than 5 cm or their objects changed, and stamped when they are. They are
built lazily, only when used: a search builds the stale regions along the
straight line to its goal (and one either side), a look ahead those of the
stretch it sees; erosion far from any actor costs nothing. A new map is
built whole at once. Only if a region along that stretch was stamped
since its last look, or has running water, does it check the cells, and
only if the way is actually blocked does it search again. A change further
on is found when it gets there, like it would be. Stuck for three seconds
without getting half a meter further, it searches again, and gives up
after three tries. What happens comes back as events (arrived, as near as
it gets, no way), shown on the screen for now.

For finding out why an actor goes where it goes, every search is logged:
who, why (sent, blocked ahead, stuck, a long path going on), from where to
where, the result, the waypoints, how many surfaces it looked at and how
long it took. When the goal cannot be reached, the start and the goal are
described: the surface, the room above it and which object takes it, the
steepest step to a neighbor against the actor's reach, the water. The same
when the path ahead turns out blocked (where, and by what) and when the
actor is stuck. It does not test every step it takes: it follows its path
(the rules for where it can go live in one place, the navigation), and
`actors_mark_path_dirty` makes it search again at once, for the physics to
call when something hits it or falls across its way. Caught by water too
fast to get out of (no path at all), an actor keeps its goal and tries
again every second, letting the water carry it meanwhile; it gives up only
once it is on dry ground and still has no way. F8 shows the navigation over
the terrain: objects in the way red, too little room orange, too steep on
every side yellow (faint yellow: on some sides, so only along the slope),
object tops to stand on blue, water it cannot pass violet, and the moving
actors' paths in green with white waypoints, as the first actor type in
actors.ini sees it. Only solid materials count: leaves (textures cut out
by alpha) and passable ones (a tree's branches) are left out, so actors
brush through bushes and under crowns.

On slopes an actor leans with the ground under its feet: the terrain is
sampled at the front, back, left and right of the feet (the lowest tenth
of the model in its rest pose) and the actor tilts by the slopes between
them, up to about 35 degrees, and stands on the plane they make.
`ground_align` (actors.ini) says how much: 1 for animals on four legs, 0
for people, who stay upright and stand on the lowest of the samples so no
foot floats.

Then the feet are put on objects (inverse kinematics). A leg is a foot
bone and the two above it (thigh and calf): every bone whose name starts
with foot, or those `ik_feet` names (actors.ini; `ik = false` turns it
off). Each frame the surface under each foot is looked up where the
animation put the foot (the navigation's surfaces, to the quarter meter),
with its slope over 30 cm; each foot looks near the height it was on, and
where it is over the map comes from the yaw alone (the lean must not move
it, or a foot by a rock's edge goes on and off it every frame). On the
terrain nothing more happens: the body leans with the slope and the legs
move as animated (bending them for every bump of the terrain made a bear
walking on a slope look drunk). On an object's top (a deck, a boulder)
the leg bends: the foot goes as high above that top as the animation
lifts it above the body's own ground plane at the same point, so a step
stays a step and an even deck changes nothing; the thigh and calf bend in
the plane the knee was bending in (two bones, solved exactly; a target out
of reach straightens the leg towards it), and a foot that is down turns
with the top's slope, up to about 35 degrees, less as it lifts (15 cm up:
not at all). Stepping on and off, the bending fades in and out. The body's
height and lean and the feet's ground follow what is found with a time
constant of 80 ms, so a foot stepping onto a boulder does not jerk it.
Swimming, the feet let go as the water deepens. It costs a few hundred
operations per leg.

On slopes an actor also walks slower uphill: its speed is divided by
1 + 2 x the rise per meter (on 20 degrees, 0.36: 58 % of its speed), and
the walk plays at the pace of its speed (there, at 0.58).

The surfaces for the feet are the navigation's, so what is not a surface
there is not stepped on. Objects under `navigation_object_meters` across
([simulation], 0.3 m) are walked through, pebbles. Small objects, up to
`navigation_small_object_meters` (0.8 m), a stone, are no surface either
and take no room: each cell keeps the height of the highest one in it,
and each actor decides by its size. Lower than its step, it walks over
it; lower than itself, the cell is passable but costs
`small_object_detour_meters` (10) more, so it goes around when the detour
is shorter than that and through it when it must (a field of stones);
taller than itself, it is a wall (a rabbit and the same stone). Pulling
the path tight never takes it through a stone the path went around. F8
shows those cells cyan (red when they are walls for the actor). Objects
bigger than that are surfaces (stood on, the feet on them) or take room,
as before; a top covering less than a quarter of a cell (a pointed rock's
tip) is no surface, so such rocks are gone around. On the 1 km map that is about 23 000 objects in the
navigation (the small ones apart); with the finer sampling the navigation
of that map is built in about 0.55 s.

An actor plays one action at a time; switching crossfades from the old
pose to the new over `blend_seconds` (actors.ini). Looping actions repeat;
an action that plays once goes back to the type's `idle_action` when it
ends, except the ones named die..., which hold their last pose. Poses are
computed on the CPU every frame (the clip's two nearest frames,
interpolated), as one matrix per bone.

Actors cast shadows only into the near shadow map. The shadow maps are
cached (the sun does not move), so while actors stand within the near
map's square it is redrawn every `actor_shadow_update_seconds` ([render],
0.1 s by default) for their shadows to follow: that redraws the terrain
and objects around the target too, so it costs. 0 turns it off (their
shadows then stay where they were last drawn).

### Flyers

A type with `flies = true` (the falcon) gets around in the air. It starts
on the ground; sent somewhere, it takes off and follows a flight path. The
navigation keeps, per cell, the highest thing there: the terrain, or the
top of any standing object, whole (a tree's crown too, from its box; loose
objects aside). A flyer goes anywhere under its ceiling, the higher of the
ground where it starts and where it goes plus `fly_ceiling_meters` (150):
its path is the straight line when nothing on it rises higher, else A*
over blocks of 4 x 4 m around what does (a mountain range), climbing
costing a little, pulled tight. Searches are cheap: a detour around a ridge
took 450 blocks and 0.2 ms.

How high it flies is its own: a cruising height above the ground, picked
at random between `fly_height_minimum_meters` and
`fly_height_maximum_meters` (15 and 60) every 4 to 12 seconds, so it is
sometimes up in the sky and sometimes comes down; never less than what it
sees ahead plus 5 m (three seconds of flight ahead: it climbs before a
ridge, not into it). It climbs at `climb_meters_per_second` (4) and dives
twice as fast, points its nose where it goes and banks into turns (the lean
that turns it at that speed, up to 45 degrees). It flies at
`fly_meters_per_second` (12), slower in tight turns, flapping (its walk
action), gliding down (`glide_action`) when it sinks.

At the goal it circles over it (`circle_radius_meters`, 12), going up and
down with its cruising height, if the ring around it is clear low enough:
within `fly_height_maximum_meters` of the goal's ground. If not (a narrow
valley, a dense forest), it comes down on the nearest spot within 40 m
that is dry, open (nothing standing there or within 1.5 m) and has room
for it: straight in along a glide of 0.4 m down per meter, or round over
the spot, sinking, when it is too high for that. On the ground it plays
its idle action. Water reaching it there (a flood) makes it take off and
circle, or land again on the nearest dry spot. Dying (an action named
die...) in the air, it drops.

Eternal Lands' birds fly by their animations: the root bone lifts the body
a meter or two. A flyer is drawn that much lower, so it is where it is and
stands on the ground. The falcon came from Eternal Lands' bird_falcon.xml
with two clips the file does not use added (`bird_fly2.caf` as glide,
`bird_idle1.caf` as hover); it has no animation for sitting, so on the
ground it plays its fight idle for now.

### The .pact format

A `.pact` file holds everything about one actor: mesh, skeleton,
animations (sampled at 30 per second), the names of its actions (walk,
idle, die... each a clip, looping or played once) and its texture,
already decoded (phones have no DXT). The layout is in
`engine/actor_format.h`. The game reads only this format; other formats are
converted on the desktop:

```sh
make tools
tools/potop_convert path/to/actor_defs/bear_1.xml data/actors/bear.pact
tools/potop_convert --dump data/actors/bear.pact
```

The converter reads Eternal Lands actor definitions: the XML names the
Cal3D skeleton (.csf), mesh (.cmf) and animations (.caf), and the DDS skin.
Their paths are relative to the Eternal Lands data directory, by default
the parent of the XML's directory (`--root DIR` sets it); a file not found
there is looked for by name next to the XML. Only binary Cal3D files of
version 700 are read so far. On the way it turns the axes (Z up to Y up),
sorts the bones parents first, keeps the four heaviest bone influences per
vertex and checks the skeleton against the file's own bind pose (it prints
the largest difference; anything above a millimeter means something was
read wrong).

### Plants (potop_plant)

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

### Ground textures from nothing

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

### The plant editor

`tools/plant_editor [plant_plans/name.plant]`, run from the project's
directory: on the left a plant's recipe, on the right the plant, grown
again at every change and drawn with the game's own object shaders
(`tools/plant_preview.c`: the same alpha test, lighting, wind; no shadows
yet) on a patch of ground with a line every meter. The left button (or the
wheel's) turns round it, the right one moves the eye up and down, the wheel comes nearer.

A recipe (`tools/plant_grow.h`) is the kind, the seed, the pictures and the
numbers, kept as text in `plant_plans/` (sources, like the pictures: not in
`data/`). The panel: Recipe (New, Open, Save, Save as; where the object
goes; the atlas), Kind (the seed, Dice for another, the height), Shape (the
branches, leaves, twigs, the crown's shape above, the kind's again), Pictures (the bark, the leaves,
twigs, fruit or flower heads: picked with the system's file dialog, which
needs zenity or the desktop portal, or typed and added; Months shows what
a picture shows each month, those found by name and those picked by hand,
Pick... for one of a month's own), Fruit, View (the month, the sun, the wind
(`--wind 0..1`) and the plant's own sway, flutter and stiffness, saved into
the object, the sky's light and the
exposure: fixed, not following the picture as the game's does, so plants
compare under the same light), Variants (Save them: the
same plant with the next seeds as `name_1.pobj`, `name_2.pobj`...).

Until Save, pictures new to the atlas are in it in memory only; Save writes
the recipe, the atlas (only what changed) and the object, and a running game
shows the plant at once. `tools/potop_plant --recipe FILE` grows from a
recipe too (the options after it change it; `--save-recipe FILE` writes the
one the options make).

### Props (potop_prop, prop_editor)

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

### Houses (potop_house, house_view)

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

### glTF (.gltf, .glb)

glTF 2.0 files (Blender, Sketchfab, AI model generators, most asset sites)
convert into actors or objects; what is made follows the output's
extension:

```sh
tools/potop_convert --list model.glb                          # what it holds
tools/potop_convert --height 1 --ground fox.glb data/actors/fox.pact
tools/potop_convert --ground helmet.glb data/objects/helmet.pobj
tools/potop_convert --ground helmet.glb elsewhere/helmet.e3d  # for Eternal Lands
```

* `.pact`, an actor: the first skin's joints are its bones (64 at most),
  every animation a clip (sampled at 30 a second; linear, step and cubic
  spline keys), every clip an action named after it in lower case
  (Blender's "Armature|Walk" becomes `walk`), looping if its name says so
  (walk, run, idle, swim, fly...). Where the clips have no `walk` but one
  has walk in its name, that one is also `walk`; without an `idle`, the
  first clip is also `idle`. `--action NAME=CLIP[:loop|:once][@SECONDS]`
  (once or more) sets the actions instead; CLIP is a clip's name or
  number. A model without a skin becomes an actor of one still bone.
* `.pobj`, an object, the game's own (see `engine/object_format.h`): the
  meshes as they stand at rest, with tangents, one material per glTF
  material (16 at most), each with its base color, normal map,
  occlusion-roughness-metal and emissive, all in the one file. Give it a
  section in `objects.ini` as any object: `model = helmet` finds
  `objects/helmet.pobj` before `objects/helmet.e3d`.
* `.e3d`, an Eternal Lands object: the same meshes, the base color alone,
  each texture written next to it as a PNG (`helmet.png`, or
  `helmet_0.png`, `helmet_1.png`...). The game reads these too.

Each material's maps come out the size of its base color texture, its
factors already in (textures PNG or JPEG, inside the file, next to it, or
as a data URI): the base color (a material without a texture becomes a
small square of its color), the normal map (its scale in), occlusion,
roughness and metal in one texture (R, G, B, as glTF packs them; the
occlusion strength in) and the emissive color, where there is one. Actors
(`.pact` version 2) carry the same maps packed like their base color, and
their tangents.

Objects and actors are shaded physically (Graphics → PBR shading,
`pbr_shading`, on unless set): the normal map bends the normal in each
vertex's tangent frame, roughness and metal shape the light (Lambert for
the diffuse part, GGX with Smith's shadowing and Schlick's Fresnel for the
sun's reflection, the sky and the ground around reflected through Karis'
analytic fit of the split sum), occlusion darkens what the surroundings
light, and the emissive map adds the surface's own light. Models without
those maps (every `.e3d`, `.pact` version 1) get neutral ones: a flat
normal, not metal, no light of their own and no reflection at all (the
neutral map's alpha says so: their textures were painted for diffuse light,
and a sheen of the sky turns dark needles gray blue), and tangents made
from their texture coordinates; they look exactly as they did. Such
materials skip the physically based shading altogether (the maps are not
read, the light is the plain diffuse one), so they cost nothing more with
PBR shading on; only materials with maps of their own (`.pobj`, `.pact`
version 2 with maps) pay for it.
Off, everything is plain diffuse, as before. Opaque materials drop their
alpha; cut out (mask) ones keep it at their cutoff, and blended ones at a
half (the game draws nothing half transparent). An actor has one texture:
several are packed side by side, each with its edges repeated around it
(texture coordinates outside 0..1 that repeat a texture are clamped then,
with a warning). Normals the file lacks are made smooth from the
triangles; more than four influences per vertex keep the four heaviest.

glTF has Y up, meters and +Z forward, as the game, so nothing turns unless
asked. Joints scaled in the file (an armature at 0.01, as Mixamo's come)
are fine: each joint's world matrix is split into a rotation and a
translation for the game's bones, and its scale goes into the inverse bind
pose.

| Option | What it does |
|---|---|
| `--scale S` | sizes times S |
| `--height M` | scaled to stand M meters tall at rest |
| `--up z` | the model has Z up |
| `--turn DEGREES` | turned around the vertical (actors face +Z) |
| `--ground` | its bottom at 0, its middle over the origin |
| `--in-place` | the root bones do not travel forward or sideways (walks that move the model) |
| `--max-texture N` | textures halved until no side is above N (2048) |
| `--no-collide A,B` | `.pobj`: these materials (names as `--list` shows them) are passable: drawn, not in the way (branches); cut out ones never are |

Checked on Khronos' samples: Fox (skinned, three clips), CesiumMan,
RiggedFigure, DamagedHelmet and BoxTextured.

Actors are skinned on the GPU: each bone's matrix (pose times inverse bind
pose) goes to the vertex shader as three rows of a 3x4 matrix, up to 64
bones, and each vertex blends four of them (`actor_skinning.glsl`). They
use the objects' fragment shader, so the light, shadows, fog and the view
under water are the same.

## Physics

Objects are rigid bodies in Box3D (see Third party); the water pushes them.
Box3D knows nothing about the water: every simulation step the game works
out what the water does to each body and hands it over as forces.

The physics runs at a fixed rate (30 steps a second) while frames come
faster, so moving bodies are drawn between where the last two steps left
them (position and rotation blended); otherwise a pushed barrel would jump
every few frames. A moving body found under the ground is put back on top,
and the log says what it was, how fast it moved and spun, and whether the
ground tile under it existed (for tracking down why).

* **Buoyancy:** each body has 9 to 27 sample points spread over its shape,
  each standing for a share of the volume it displaces (its mass over its
  density). A point under the surface pushes up with the weight of the water
  its share displaces, at that point, so logs roll and boats tilt the way
  they float. What floats and what sinks follows from the density alone.
* **Drag:** water flowing past a point pushes it with
  1/2 * water density * `drag_coefficient` * area * speed squared, against
  the speed between the water and the point. Under water things also stop
  spinning quickly.

Which objects take part is set per type in `objects.ini` with `physics`:

* `none`: only drawn (candles, cobwebs).
* `flood`: no body, but flowing water lays it down in the flow's direction
  (`flood = flatten`) or tears it out (`destroy`) once it flows faster than
  `flood_speed_meters_per_second` in water at least `flood_depth_meters` deep.
  Flattened plants stand up again over `flood_recover_seconds` once the water
  is gone. No type uses it now: a flood rarely flattens grass for good, and
  doing it at once looked wrong. Grass, flowers, reeds and crops are `none`.
* `static`: a solid body that never moves: houses, tents, walls, docks, wells,
  and also trees, bushes, cacti and fences (a river in flood does not tear
  out a forest; the anchor stays in the code for a real catastrophe later).
  Static things block the water (`blocks_water`, on by default for them; off
  for trees, tents, wells, docks, bridges, fences and signs): they are
  written into the water simulation as walls, the highest point of the model
  over each cell (a pointed roof sheds the water), so a flood splits around a
  house, or rises over it. A wet film on a roof is not drawn.
* `body`: a solid body that moves: logs, barrels, crates, haystacks, carts,
  boats, rocks. It lies loose, asleep, and the water takes it as soon as it
  can. With `anchor_newtons` it would stand still until the water or a hit
  pulls harder than the anchor, then come loose (trees tip over,
  `falls_when_uprooted`); nothing uses that now.

The collision shape is made from the model (`shape = auto`): a capsule for
long things (logs, boats, planks), a convex hull of the model otherwise;
`sphere`, `capsule`, `hull` or `mesh` (the model's own triangles, static only)
override it. Standing objects collide with their solid triangles only:
not leaves (cut out by alpha) nor passable materials (`OBJECT_MATERIAL_PASSABLE`:
a plant's branches, `potop_convert --no-collide`); one with nothing solid
(a flower) is in no one's way. Loose and floating, the shape wraps it all. The mass comes from `mass_kilograms` (at size 1, with the size
cubed) or from the shape's volume times the density.

Breakable things (`damage_newtons`) go from intact to damaged to destroyed
when the water or a hit pushes harder than that, with `damaged_model` and
`destroyed_model` if those exist (a destroyed one without a model is gone),
throwing out `debris_count` pieces of the `debris` types. Nothing is
breakable yet: the damaged and destroyed models do not exist. Houses are
pushed from the outside (they keep the water out): the difference of the
water's weight on opposite sides plus the flow against them.

Bodies cost next to nothing while they sleep: a sleeping body is only
looked at when the water around it moves (an active water tile), and Box3D
wakes whatever a moving body hits. A loose object is drawn where its body
is, kept out of the 32 m grid (culled one by one), and stays where it ends
up, on land too, until the player does something about it; one that leaves
the map is gone. The ground is a Box3D height field per terrain chunk, made
only where something moves; tools and stamps redo it at once, erosion only
where it changed the ground more than `terrain_resync_meters`.

A 1 km map has about 24 000 bodies (trees, bushes, rocks, buildings, things
lying around): made in a quarter of a second, about 0.3 ms per step while
everything sleeps. Saved maps keep what the physics did (loose objects where
they lie, flattened plants, damage).

The **Object** tool (Nature in the brush panel) places one object of a
chosen type per click. Loose things fall from the "Drop from" height (3 m to
start with), awake; at 0 they are put down asleep.

## Decals

Marks laid on whatever is there: a bullet's hole in a wall or a tree, its
hit in the dirt, the ground scorched round an explosion, a splash of mud, a
damp stain, soot (`engine/decals.h`, `data/decals.ini`). Each is a box laid
on a surface; what the box takes in of the ground, the buildings, the
props and the objects gets the decal's picture over its color, roughness and
relief before it is lit (a hole is dark in sun and shade alike, a wet stain
shines). They go where the local lights go: sorted into the same clusters
on the CPU each frame, in the same buffer, so no shader needed another
texture slot (the terrain's has none left); their atlas is in that buffer
too, filtered by hand.

* `data/decals.ini`: the kinds (cell of the atlas, size, depth, opacity,
  relief, roughness, life). `data/impacts.ini`: `decal` and `decal_size`
  per kind of hit. Explosions near the ground leave a `scorch`.
* The atlas: `props/textures/decals_color.png` (RGB color, A coverage) and
  `decals_normal.png` (RG relief, B roughness), 512 x 512 in 4 x 4 cells;
  without them the game draws stand-ins (cells 0..7: hole in wood, two
  holes in stone, a hit in the dirt, scorch, mud, damp, soot).
* At most 512 (the oldest goes); drawn to 80 m, fading out over the last
  third; `[render] decals = false` turns them off.
* Console: `decal <kind> [size]` lays one where the middle of the screen
  looks, `decal clear` takes them all away.
* They stay where they were laid: loose bodies and actors get none from the
  projectiles (a decal does not follow what moves).

## Particles

Fire, smoke, sparks, magic: the effects are sections of
`data/particles.ini`, which documents every key at its top (texture, blend,
continuous or burst, the emitter's shape, life, speed, direction and spread,
gravity, drag, turbulence, spin, stretch, collisions, flashing, and color and size
gradients over a particle's life). The **Effects** tool (Debug, in the brush
panel) lists them: a click plays the chosen one on the first thing the ray
under the pointer meets (as Alt shows it): an actor (it follows the actor),
else an object (fitted to its box;
`shape = outline` goes around it, like the sparkles of a finished building;
it follows the object if the water carries it off), else on the ground.
"Stop all" ends them, "Reload ini" reads the file again while they play.

Everything runs on the GPU, in compute shaders (`data/shaders/particles_*`):
a pool of 262 144 particles (31 MB), a list of free slots and two lists of
living ones used in turn. Each frame the CPU sends only "this many of effect
E, here, shaped so"; new particles take free slots, every living one ages,
moves (gravity, drag, swirls, bouncing off or dying on the ground and the
water, read from the textures the terrain and water already have on the
GPU) and is drawn as one quad facing the camera, the count straight from the
GPU (indirect draw), nothing read back. Glowing particles add to what is
behind them and lit ones (smoke) cover it, in one draw with one blend mode,
unsorted. Particles fade where they go into the ground or the water instead
of ending in a hard line. One thinner than a pixel (a tracer far off) is drawn a pixel wide and as much dimmer, so it does not fall between the pixels. The textures are made at start, no image files: a
soft dot, a spark, a star, a ring, four smoke puffs and eight flame frames. An effect with `light` lights its surroundings too (see Local lights,
under Tuning).

## Weapons

On foot, **1** brings the first weapon up (or puts it away), **2** the
second (the one in hand goes down first); while one is up, the left button
fires, the right button (held) aims and **R** reloads (not the rain). The status line shows
the weapon and what is in the magazine (the ammo is endless for now). There are two for
now, drawn from boxes and prisms until there are real models: an AK-47
(`model = procedural:rifle`) and an SVD with a scope
(`procedural:scoped_rifle`).

Aiming brings the weapon to the middle, its sights at the eye
(`aim_seconds`), and narrows the view `aim_zoom` times (1.35 for the AK);
the mouse turns the view as much slower, the spread and the recoil are
`aim_spread` and `aim_recoil` times theirs. A weapon with `scope_zoom`
(6 for the SVD) shows, once at the eye, the scope's round view (black
round it, a duplex reticle) narrowed that many times; the weapon itself is
not drawn then.

Three files, each documenting its keys at its top:

- `data/weapons.ini`: one section per weapon: its ammo, `fire = semi` or
  `auto`, rounds per minute, magazine, reload and ready time, spread (how it
  grows shot by shot and comes back), recoil (up and sideways, kicking the
  view), the muzzle's effect, the tracer's effect and how often (every Nth
  round), the model.
- `data/ammo.ini`: one section per kind of ammo: muzzle speed, mass, drag
  (1/m: the air's slowing goes as drag × speed²), damage, push (how hard
  it shoves a loose body: its momentum times this), how long it may fly.
- `data/impacts.ini`: what a hit throws up, by what it hit: the ground's
  materials, the objects' categories, the water, with one fallback for the
  ground and one for objects.

The effects are sections of `data/particles.ini`: `muzzle_flash`, `tracer`,
`impact_dirt`, `impact_rock`, `impact_wood`, `impact_water`. A tracer starts at the muzzle as drawn, beside the bullet's way: from the eye, a streak going straight away would be only a dot.

The bullets are real projectiles (`engine/projectiles.c`, up to 512 at once):
gravity, the air's drag against the wind (the plants' wind, 0..1 as up to
10 m/s), and the water, which stops one within a meter or two. Each step
(at most 1/120 s) the way flown is checked against the heights and against
the physics' shapes, so a fast one does not go through a thin trunk; a
loose body it hits is pushed. Each hit is also kept for the game (what, where, how fast, the damage as
the energy left goes: `projectiles_take_hits`); what the damage does is up
to the game. Skills change a weapon through `weapons_set_modifiers` (damage,
spread, recoil, reload time, fire rate, muzzle velocity, a bigger magazine).

The weapon in hand (`engine/viewmodel.c`) is drawn last, in the camera's own
space with its own field of view (52°) and its depth squeezed into the
nearest sliver, so it never goes into a wall. It comes up and goes down,
kicks back with each shot, dips for a reload and bobs as you walk; lit by
the sun and the sky. The crosshair is drawn with it.

`--map-test` checks that the weapon fires as fast as weapons.ini says and
that a bullet fired down from 300 m lands on the ground (or the water on it).

Actors are hit by their bodies as posed: each bone has a box (in its own
space, around the vertices it moves most, made when the model loads) that
moves with the animation; the bullet stays in the first one it meets
(`impact_flesh`, the `[flesh]` section with `actors = true`). The hit says
which actor, which bone and where on the body by the bone's name (head:
head, mouth, jaw, ears, eyes; limb: legs, arms, wings, tail, feet, toes;
else body). `actors_hurt` plays its `pain...` action (then back to what it
did), `actors_kill` its `die...` action: it stops for good, a flyer drops
out of the sky. What a hit does is the game's; the game (game/) stands in for
it: 100 health (flyers 30), a head takes 2.5 times the damage, a limb 0.6,
each hit flinches, none left kills (the log says each hit). Eternal Lands'
animals have the flanks on the leg bones, so many body shots count as
limbs.

`--map-test` also shoots a bear from 4 m and checks the bullet stops in it
and it dies.

Not yet: decals (bullet holes), sounds, melee.

## Inventory

What can be carried is `data/items.ini` (it documents its keys at its
top): one section per item, its name, category (a small sign before it in
the lists: weapon, ammo, food, material, clothing, tool, quest, money,
misc), weight, value, how many go in one row (`stack`), the body's slot it
is worn or held in, and what it is to the weapons (`weapon = ak47`: this
item is that weapon; `ammo = 7.62x39`: one of it is a round). The body's
slots are its `[slots]` section (hands, head, body, legs, feet, back).

A container (`engine/inventory.h`) is a list of rows (an item and how many)
with what is worn in the slots and a weight limit the game sets
(`inventory_set_max_weight`; 0 none). The player's pack, a dead animal's
loot, what lies on the ground: all containers. `inventory_add`,
`inventory_remove`, `inventory_count`, `inventory_move`,
`inventory_move_all`, `inventory_equip` and `inventory_unequip`, and
events for what goes in and out (`inventory_poll_event`: a quest watches
those; the game writes them to the log).

The weapons load from the pack: a reload takes the rounds there are
(`weapons_set_reserve`, `weapons_take_loaded`), the status line shows them
after the magazine (`AK-47 23/30 +120 auto`); 1 and 2 bring a weapon up
only if it is carried, and it goes into the hands.

On foot, **I** opens the inventory: the body's slots on the left (a click
takes a thing off), the rows on the right (a click chooses one: Use,
Equip, Drop, Drop all; a double click uses it), the weight over them. An
item with `usable = true` can be used: that is an event
(`INVENTORY_EVENT_USED`, `inventory_use`), what it does is the game's (the
game eats food). **E** acts on what the eye is on
within 2.5 m: a dead animal is looted (its rows next to yours; a click
moves a row across, Take all), a thing on the ground is picked up (what
fits). The pointer is free while a window is open; Escape or walking off
closes the loot.

What is dropped lies on the ground as an object (`engine/ground_items.h`):
the item's `model` names an objects.ini type (else a sack or a crate),
made as big as its `size`; a loose body like the rest (it falls, floats,
sinks, is pushed by bullets). The map's file keeps what lies about and the
player's pack (`items.bin`, by the items' names).

The game plays its part: a 40 kg pack with both rifles, their
rounds, bread, apples, coins, a knife and boots (the AK and the boots
worn); bears leave a pelt, meat and claws, falcons feathers and meat.
`--map-test` checks the weight limit, moving, wearing, and a thing laid
down coming back with the map and picked up.

### Picking things off the map

An object can be picked by hand (`engine/harvest.h`): **E** on foot on it
gives its item into the pack. objects.ini, per type or in a category's
`[defaults]`:

| Key | What |
|---|---|
| `harvest` | the item it gives (items.ini's) |
| `harvest_count` | how many a time: `2`, or `1, 3` (at random between) |
| `harvest_removes` | `true`: it goes once picked (a weed, a flower) |
| `harvest_times` | else how many times it can be picked (a willow: 2 twigs), then "Nothing left to pick" |
| `harvest_regrow_hours` | game hours after which it can be picked again (0: never; it needs the day cycle on) |
| `harvest_change_object` | the type it turns into while picked out (a bush without its berries), back once grown |

The game can set or change it per type (`harvest_set`). Each object keeps
how many times it was picked and how long until it grows again, and so
does the map's file (`harvest.bin`, each by its type and place). The
game, where objects.ini says nothing: tomatoes (3 times, 1 to 3 a time,
again after a day), willows (twigs, twice, again after two days), flowers
(they go). `--map-test` picks one out, turns it and lets it grow back,
keeps it as a file does, and picks a weed.

## Compass, markers and the map

Markers (`engine/markers.h`) are places shown on the compass and the map:
`markers_add` (where, a color, a name), `markers_remove`, `markers_move`,
`markers_set_active` (the objective followed). The journal puts the
tracked quests' objectives (see Journal); the player puts waypoints of
their own on the map (one put is followed, no objective in focus then).

On foot the compass runs across the top: the letters and ticks turn with
the eye (north is -Z, the map's top), each marker a dot of its color (at
the edge with an arrow when it is out of view), the active one ringed with
how far it is.

**M** on foot opens the world map: an artist's picture if there is one
(`data/maps/NAME.png`, NAME the map's saved name without `.active` or
`.baked`, else `data/maps/world.png`; north up, covering the whole map),
else one made from the land (`engine/world_map.c`: the relief shaded from the
north-west, the ground's materials in muted colors, the water blue by its
depth, the woods darker, on a paper tone; made again when opened if the
map changed or the last one is half a minute old, about 70 ms for 1024²).
The markers with their names, the player as an arrow the way they look.
The wheel zooms (about the pointer), a drag moves it, a right click puts a
waypoint (followed at once), a right click on it takes it away.

## Journal and notices

The engine shows the quest log; the game keeps its quests (their flags,
why an objective is done) and tells the journal (`engine/journal.h`) what to
show: `journal_set_quest` (an id, the title, the description, the state:
active, inactive, completed, failed), `journal_set_objective` (a line
each: open, done or failed, how far along, "3/10"), `journal_set_place`
(where it is on the map), `journal_remove_quest`, `journal_remove_objective`.
The player tracks quests (their open objectives with a place are shown on
the compass and the map) and puts one objective in focus (the active
marker, with how far it is); how many may be tracked is the game's
(`journal_set_track_limit`, 3). What the player chose comes back as events
(`journal_poll_event`), if the game cares.

**J** on foot opens it: the quests by state, the newest first, a gold dot
on the tracked ones; the chosen one's title, state, story and objectives
(a ticked box, a crossed one, an empty one; the focused line gold), Focus
and Map on each open one with a place (Map opens the map on it), Track or
Untrack.

`hud_notify(hud, text, seconds, style)` shows what the game says for a few
seconds under the compass (info, quest in gold, warning in red), newest
last, fading out.

The game plays a quest, "The Hunt", in C: shoot a bear (the
place follows the nearest living bear), gather five falcon feathers (the
nearest falcon; how many from the pack), then an objective not shown
before: bring the bear pelt to the camp (a place a little way off); there
it takes the pelt, gives 150 coins and is completed. `--map-test` checks
the track limit, the markers, the focus, and the markers going with done
objectives and untracked quests. The journal is not saved yet (the game's
saves will keep its quests).

## Dialogs

The engine shows a conversation, the game holds it (`engine/dialog.h`):
`dialog_open` (who: an actor and a name), `dialog_say` (their line),
`dialog_set_choices`, `dialog_add_choice`, `dialog_remove_choice` (the
player's answers, each with the game's id, unique in the conversation, so
they can change while it goes on; none: a line to read and Continue, id
0), `dialog_close`. What the player picks comes back as an event
(`dialog_poll_event`: the answer's id, or that they closed it), and the
game says the next line: its trees, conditions and flags are its own C
code. The window sits low in the middle, in a bigger font (`dialog_text_scale`
times the interface's, 1.6; Settings > Interface: 2 or more for a
television across the room) on a see-through ground (80%): the name in gold, the line,
the answers (a click, or 1 to 9). While it is open the pointer is free and the
player stays put; Escape or walking off (6 m) closes it.

Actors can have a name the game gives them (`actors_set_name`; enemies'
in red): it is shown over their heads within 30 m. The game knows its own
actors by its numbers for them (`actors_set_game_id`, 0 none;
`actors_find_by_game_id`): a quest's wolf is that wolf, not any wolf. Their
ids are only for while they live in memory; the numbers are kept in the
map's file with the actors (`actors.bin`: type, place, name, number, enemy
or not, dead or not; a map from before them leaves the actors there are).
The hits only report the damage (`projectiles_take_hits`: which actor,
where on it, how much, who shot); whether it flinches or dies is the
game's (`actors_hurt`, `actors_kill` only play it).

The World window has what K gives the actors it puts: a name, the game's
number, enemy or not. `actors_look_at` turns
one standing to look at a place (a talker at the player),
`actors_stop_looking` lets it be.

The game's talker is Moș Martin, an old bear put down a few meters ahead
the first time one is on foot. **E** on him talks: who he is, the other
bears (he knows if you have shot any), and his honey, which he trades for
three tomatoes: a small quest in the journal (the place is Moș Martin, the
count from the pack), done when you bring them (a jar of honey). Shooting
him only makes him cross. `--map-test` checks the dialog's events.

## Picking and highlighting

`objects_raycast` lists the objects a ray goes through, nearest first. It
looks only in the 32 m grid cells along the ray (and their neighbours: a
house or a tree reaches over from the next one) and at the loose objects;
for each, a ball around it first (most are nowhere near), then its box in
its own space, then its triangles, so a tree's crown counts and the air
around it does not (tens of microseconds for a 50 m ray over a wooded map).
`actors_raycast` does the same with the actors' boxes; the game merges the
two lists by distance.

Two kinds of ray: `OBJECTS_RAY_ALL` sees everything, grass too (Alt, the
editor's tools such as Effects), `OBJECTS_RAY_INTERACTIVE` only what the game
can do something with: objects with `OBJECT_STATE_INTERACTIVE`, actors with
`interactive`. What interacting means is the game's business; the engine
only knows the flag. Until the game sets it through its API, placeholder
values: objects with a physics body or in a layout, and every actor. On
foot, what the walker looks at within `interact_meters` ([tools], 6 m) and
is interactive glows softly blue, when `interact_highlight` is on (off by
default; Settings, Camera: "Glow what is in reach"). The flag is not saved with
maps yet.

One object
(`objects_set_highlight`) and one actor (`actors_t.highlight_id`) can be
drawn highlighted: a glow strongest at their outline, pulsing, in a color
of choice, in the normal drawing pass (an object's instance data carries the
flag; no second pass). Alt uses it for debugging; the game will use it for
selection.

The code side of particles: `particles_play(particles, effect, x, y, z)` or
`particles_play_on(...)` with a box, `particles_stop(id)`.

## Layouts

A layout is a group of objects placed together around a center point, read
from `data/layouts.ini`: a homestead is a house with a fenced yard, a garden,
an outhouse and odds and ends; a camp is a fire with tents around it (and
sometimes a fortune teller); a hamlet is a well with homesteads around it,
their doors towards the well. Parts are placed in the layout's own frame, +z
being its front: single objects, rings (facing the center, outwards, along
the circle), scatters, garden rows, and fences around a rectangle with a
gate. A part can be a whole other layout. Models face where their
`front_degrees` (objects.ini) says, so doors and tent openings look the right
way.

Placing a layout levels the ground under it if it asks to (the middle flat,
a soft edge), clears the vegetation under what it places (the yard keeps its
grass, the garden does not), and puts everything into one group: Delete
removes the whole layout. Layouts can face down the slope or towards the
nearest water; fishermen put their dock where the water starts in front of
them, with boats on the water (boats and docks float).

New maps get layouts where the ground suits them: soil, height, how flat the
footprint is, no water on it, room from the others. Generated maps are dry,
so layouts that need water (fishermen) only come with the layout tool.

Stacks (`part.N = stack`) pile copies of one model, optionally on two
supports across the pile's ends: `log_pile` (five logs on two logs) and
`crate_stack` (two piles of five crates) stand in homesteads now and then,
and the layout tool places them anywhere. They sleep until something knocks
them over.

## How it fits together

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

## How the water works

Every cell holds a column of water and is connected to its four neighbors by
"pipes". The flow through each pipe has inertia: a difference in water level
accelerates it, friction (stronger in shallow water) slows it down. That
inertia is what makes waves, overshoot and sloshing. A cell never sends more
water than it holds: if its four pipes together would, they are all scaled
down. Deep water makes waves fast, so the step is split into substeps when
needed to stay stable.

In the classic virtual pipes model every pipe has the same cross section
(a cell squared), so a film of a few millimeters on a slope races at tens of
meters per second. By default the pipes are sized by the water that can
actually pass between the two cells (from the higher water level down to the
higher ground), up to `pipe_depth_limit_meters` (2 cm by default): thin
films flow slowly, deeper water behaves exactly as before. (Sizing them by the full depth, which is the
shallow water equations, makes big waves pile up into sawtooth fronts: their
crests travel faster than their troughs, and the equations lack the
dispersion that keeps real short waves round.) M switches between the two.

Waves in this model travel at about sqrt(g * pipe depth), and the pipes are
never deeper than a cell, so deep lakes do not need extra substeps; the step
is split only if a wave could cross too much of a cell in one.

The water itself can never move more than a cell per substep (a cell cannot
give away more than it holds): 7.5 m/s at 0.25 m cells and 30 steps per
second. When flow at least a cell deep gets close to that, the next steps
are split into more substeps, so a breached dam or a waterfall is not held
back; it costs extra only while the water is that fast. What does set the
top speed of fast water is Manning's bed friction (`manning_roughness`,
the formula hydrologists use for rivers): it grows with the square of the
speed, so half a meter of water down a 1:3 slope tops out around 10 m/s
instead of accelerating for as long as the slope lasts. The plain linear
friction (`friction`) still handles slow water and lets lakes settle.

On screen the water level is blended a little with the neighboring cells,
which hides the one cell zigzags the simulation leaves on advancing fronts
and short choppy waves; the simulation itself is not touched.

The map is divided into 64x64 tiles: dry tiles are skipped, tiles whose water
has come to rest go to sleep, and tiles wake up when water flows in, a
neighbor's level changes against them, or a tool, spring or rain touches them.

Once asleep, a tile stays asleep unless something big happens: water pouring
into one of its cells faster than `asleep_wake_millimeters_per_second`, a
neighbor's level dropping `asleep_level_wake_millimeters` below its own (a
dam giving way), a tool, spring or rain. Smaller trickles running into it are
thrown away (counted with the evaporated water), so the calm parts of a lake
cannot keep waking each other over millimeters. A tile goes to sleep after
`sleep_after_substeps` substeps of still water.

### Sky

The sky is a dome at infinity, worked out for every pixel
(`data/shaders/sky.glsl`): `sky_color` at the horizon (the same color the
distant land fades into, so there is no seam), `sky_zenith_color` straight
up, the sun's disc and glow, and the clouds. The clouds are the same moving
sheet whose shadows cross the land: a ray from the eye is traced up to it
(the sheet is curved like a small planet's sky, so it comes down to the
horizon and fades into its haze),
so the cloud overhead is the one shading the hill below (three turned layers
of the noise texture, so its repeats never line up into a grid towards the
horizon; `data/shaders/clouds.glsl`, shared with the shadows). Seen from high
up the land ends below eye level, so the sky's horizon is lowered to where
the haze swallows the land. The water mirrors
the same sky (with blurrier clouds: the ripples scatter its rays). Under
water there is no sky, only the water's color.

With `volumetric_clouds` on (Graphics → Clouds → Volumetric, the default)
the sky leaves out its flat sheet and real 3D clouds are drawn over the
scene instead (`clouds.c`, `data/shaders/clouds_*`): a layer from
`cloud_height_meters` (at least 400 m, well above every mountain) up
`cloud_thickness_meters`, with `cloud_steps` setting the step inside clouds
(clear air is crossed in steps three times longer; far away all steps
grow). Where the clouds are is still the flat cloud field, so the shadows
on the land lie under them; their shape is a 3D noise of broad billows
eaten at the edges by a finer one, computed once (then kept in the
preference directory's cache). The light: absorption towards the eye and
towards the sun (four steps), dark edges facing away from the sun, silver
linings around it (held back so they do not burn white), the sky's light
from above. The layer is a shell around a planet of 200 km radius
(`CLOUD_PLANET_RADIUS`, `common.glsl`), so the clouds come down to the
horizon and fade into its haze, as in the real sky: a layer at 1000 m meets
the horizon about 20 km away (the Earth's 6371 km would put it at 110 km,
too far to march; 40 km, as it was, at 9 km, the clouds heaped up there).
They are marched up to 80 km away. Clouds → Horizon drop
(`cloud_horizon_drop_meters`, 1000 m) lets the planet's surface down under
the eye while the layers stay where they are over it: the rays a little
under the horizon still reach the clouds, so they go on a few degrees under
its line (about sqrt(2 drop / radius): 6 degrees at 1000 m) instead of
stopping at it.

The haze (Graphics → Haze, `fog_density`) is densest at height 0 and thins
upwards, e times every Haze height (`fog_height_meters`, 1000 m): mountain
tops and clouds are seen through less of it. For the land it goes along
the straight way to the eye; for the clouds along the ray over the curved
ground, which stays low for a long way towards the horizon. Hidden clouds
fade out to the sky behind them (hazy at the horizon itself) instead of
being painted the haze's color, so they never stand out as patches of
another blue.

Clouds → Type picks a kind of clouds, which sets the values
below it (`cloud_type` in the ini: 1 small fair weather puffs, 2 medium
heaps, the default, 3 storm towers with dark bases, 4 overcast; changing
any of the values makes it 0, custom): the cover's coverage, softness and
spread (`cloud_tiling_meters`), the layer's height, thickness and density,
the billows' size (`cloud_size`), how tall the thick parts grow
(`cloud_towers`) and how dark a thick layer's bottom gets
(`cloud_base_darkness`). Inside the clouds the light is scattered many
times (three octaves of ever fainter, ever more even light), so thick
clouds stay bright inside instead of turning black.

The clouds' shapes come from an atlas made at start (`cloud_weather.c`):
thirty shapes grown out of blobs the way real clouds heap up (lumps, smaller
lumps bulging from them, and smaller still)
(fair weather puffs, cauliflowers, towers, storm clouds with an anvil
downwind, flat sheets), each kept as a small 3D distance field. They are
stamped over a map of the sky 25 km a side (it repeats, and the wind
carries it), turned and scaled at random, as many as the coverage says, as
far apart as the spread, as tall as the towers allow; the shader only
roughens their surfaces into billows. Their shadows on the land are their
own: twice a second a compute pass looks up through the layer over every
patch of the cloud map (512 a side, about 50 m each) and keeps how much of
the sun gets through; the land, the water, the objects and the people read
it where their way to the sun meets the clouds' base. Without the
volumetric clouds the flat field's shadows stay.

Clouds → Variety makes the clouds differ in how dense they are,
the same shapes white and puffy or heavy and dark (up to four times either
way; fair weather puffs lighter). Lifetime (`cloud_lifetime_minutes`, 15
unless set, 0 for still clouds) makes each one live: it forms from its
densest cores, grows whole, then dissolves from its thin edges inwards and
leaves clear sky for a while before forming again, each at its own pace;
it is all in the shader, from the clock, with the shadows following.

The Sky button opens the clouds' map seen from above, centered on you (the
red dot; the line is the wind), to paint the sky with the mouse: Coverage
(how many clouds: clear blue to cloudy white), Heaviness (puffy white to
heavy gray) and Kind (fair weather puffs green, heaps yellow, towers red),
left button more, right button less, "Back to normal" or the Normal sky
button for the sky the settings make. The middle of each range is what the
settings say, so the sliders still work over a painted sky. The clouds are
placed again a few times a second while painting; the painted sky is saved
with the map (cloud_paint.h). The fourth, Flocks, is where the flocks are
(below); it starts empty everywhere.

The flocks are a thin layer of small white clouds high up, the ones in
patches and rows (altocumulus, cirrocumulus: "mackerel sky"), all
procedural: the Worley cells of the shape noise, a little longer along the
wind, pushed off their grid by a broader noise, bunched in rows across the
wind, slowly boiling, flat at the bottom and rounded on top, thin and white.
Each has a texture of its own, sized to it: the fine noise frays its edge
into tatters, broad lumps make its middle thick in knots and thin, almost
open, between them (fading out far away, where it would sparkle).
Marched into the same dome after the big clouds, seen through them, with a
light dappling of shadow on the land. Clouds → Flocks:

| Setting | Key | What it does |
|---|---|---|
| Size | `flock_size_meters` | how big each one is: grains (100 m) to big heaps (1500 m); the layer is about a third of that thick |
| Amount | `flock_amount` | how many: a few strays to packed in rows |
| Trend | `flock_trend` | all of them at once: dissolving (-1), steady (0), growing (1); at 1 from none to all in five minutes of the sky's clock (the console's `time` speeds it up); a new Amount starts over from it |
| Height | `flock_height_meters` | the bottom of their layer (4500 m) |
| Density | `flock_density` | how opaque they are: a veil to solid little heaps (0.5) |
| Shade | `flock_shade` | how gray their middles get in their own shadow, 0 all white (0.3) |

Only where the sky is painted for them: none at all costs nothing.

`tools/cloud_bake` grows clouds instead of drawing them: a small
simulated atmosphere (air as a fluid on a grid, carrying heat, vapor and
cloud water) over unevenly warmed ground; the warm patches send up
thermals, the rising air cools, condenses into cloud at its dew point and
the latent heat lifts the cloud further, until a stable layer caps it. The
cloud is baked into the atlas's shape format, as
`data/clouds/<type>_<n>.cloud` (humilis, mediocris, congestus, storm,
stratus; n from 1 to 16): a type's simulated clouds take the place of its
shapes made of blobs. Many at once: `tools/cloud_bake --type congestus
--count 16` grows seeds 1 to 16 into `data/clouds/congestus_1.cloud` ...
`congestus_16.cloud` (`--seed N` to start elsewhere, `--out-dir` for
another directory). Each takes some minutes at `--grid 128` (the default),
on all cores. The atmosphere's knobs are on the command line
(`tools/cloud_bake --help` lists them; `--type congestus --cap-height 1800
--show` prints what a run would use and stops): ground temperature, how
fast the air cools with height below and above the cloud base, where the
stable layer that caps the clouds is, the humidity, how warm, wide and
long lived the thermals are, the wind shear. `--grid 64` is eight times
faster for trying; `--preview cloud.pgm` writes the cloud seen from the
side and from above; `--name` writes it as another type. A window shows
the cloud growing while it is simulated (`--no-view` for none): drag to
turn it, the wheel zooms, Space pauses, Enter keeps the cloud as it is
now, N drops it and goes on to the next seed, Esc stops without keeping
anything; when it is done the window stays up with the last cloud until Esc. `--save-water file` keeps a simulation and `--water file` opens
it again in the window, to look at and bake without simulating.

The simulation runs on the GPU (a compute shader,
`data/shaders/cloud_sim.comp`; `make shaders` first), many times faster
than on the CPU; `--cpu` for the CPU, which it falls back to without a GPU.
The two do not agree to the last cell, but grow the same clouds. Every
couple of hundred steps it prints the GPU's milliseconds a step, and what
each phase takes (advection, physics, vortices, pressure; timed one at a
time, so they add up to a little more).

To find good settings, the fuzzer: `tools/cloud_bake --fuzz fuzz.ini`
grows clouds with settings picked at random from the ranges in the ini
(`tools/fuzz_example.ini` to start from), and
keeps every one, good or bad, for you to judge: `fuzz/1.png`, `fuzz/2.png`
... (`--out-dir` for elsewhere; it goes on from the first free number), each
the cloud from below, from the side and from above, and next to each
`1.txt` ... with what grew it. The window shows each one growing; N cuts a
run short (it is still kept), Esc stops. The ini:

    [run]
    type = congestus     ; the type the ranges start from
    grid = 64
    runs = 200

    [ranges]
    ; any knob, without its dashes, or vorticity or seed:
    ; low, high = uniform between them; one number = fixed
    thermal-warmth = 0.5, 3
    cap-height = 1200, 3000
    vorticity = 0, 0.08
    seconds = 1500

A good one is grown again, finer, from its txt: `tools/cloud_bake --params
fuzz/17.txt --grid 128 --out data/clouds/congestus_1.cloud` (the first line
of the txt says so). Anything on the command line changes what the txt
says; the txt changes what the ini says.

Clouds made elsewhere come in as OpenVDB files: `tools/cloud_bake --vdb
cloud.vdb` reads the file's density (the first float grid, or
`--vdb-grid name`) and shows it in the window, at its own resolution;
Enter writes it for the game (below), `--picture
cloud.png` draws it from below, the side and above. `--vdb-meters` is a
voxel's size (5 m unless given), `--vdb-density` scales how thick it looks,
`--vdb-up` says which of the file's axes is up (z unless given, as these
EmberGen exports have it; y, x, or -z for upside down), `--vdb-size 128`
makes it smaller first (its longest side 128 voxels), to see
what a game sized one keeps. A read cloud is drawn lit as it would be in the
game: a sun anywhere (the arrows move it), the light scattered many times
inside the cloud (Wrenninge's octaves), the silver lining with the sun behind
it, the sky's light from above; `--sun`, `--sky`, `--exposure` for how
bright.
The reader is ours (`tools/vdb_read.c`, plain C, no OpenVDB library): float
or half float grids, uncompressed (EmberGen's "compression: none"), zip, or
Blosc (Houdini's default; BloscLZ, LZ4 or zlib inside). Houdini's files have
y up: `--vdb-up y` for them.

For the game a read cloud is written with its real density, not as a
shape: `tools/cloud_bake --vdb cloud.vdb --out-dir data/clouds --no-view`
writes the next free `data/clouds/cumulus_<n>.cloud` (`--name` for another
name, `--out file` for a given one): the density, its longest side across
192 texels at most (a smaller cloud keeps its own), each side a multiple of
4, 0..255 with the 95th percentile of the cloud at 255, zlib packed (a few
hundred KB). A file holding a field of clouds (many small ones apart, like
Houdini's Cumulus_Cloud_1) is cut into its clouds with `--split`: each piece
that touches no other is a cloud of its own, the crumbs dropped.

The game loads `cumulus_1.cloud` ... `cumulus_64.cloud` into an atlas of
their own, each at its own size (side by side in rows), compressed to BC4
on the GPU (half a byte a texel; a byte where the GPU has no BC4 in 3D):
the 22 shipped (the ten EmberGen cumulus, Houdini's congestus, eleven small
ones cut out of its cumulus field) take about 45 MB. When there are any,
every cumulus stamp of the sky uses them instead of the shapes made of
blobs: one picked at random (for fair weather puffs the flatter of two, for
towers the taller), smaller or bigger with its kind, turned and mirrored at
random; only overcast sheets keep the old shapes. They are marched as they
are, lit the same way (the shade towards the sun from short steps first,
so their lumps shade each other); near, their thin parts frayed a little by
the fine noise.

Since nothing on the map reaches the clouds, they are marched not for every
pixel but for every direction around the eye, into a dome (an octahedral
map). Each frame
marches a sixteenth of it again and blends that with what it held (its
size is `cloud_dome_size`, Graphics → Clouds → Sharpness; 0, Auto, makes it
about 1.4 times the screen's height: 2048 at 1440 pixels; with the eye
under the clouds it holds the sky above the horizon only, twice the texels
for it): turning
the camera costs nothing, the cost does not grow with the screen's
resolution, and the jittered marches average into smooth clouds. The dome
is seen from the eye alone, so while the eye moves enough for the nearest
clouds to shift (a few meters a frame under them, any move inside them) all
of it is marched each frame, coarsely, a march for every 2x2 texels; when
the eye stops the fine detail comes back within a few frames. The still
dome's refresh goes by the clock, not by the frames: all of it every
`cloud_refresh_seconds` (Refresh, 0.1 s), a sixteenth at a time (none in a
frame when none is due, a few in a slow one), each new march mixed in so
that what a texel held fades over `cloud_memory_seconds` (Smoothing,
0.3 s): the same sky at 30 and at 400 frames a second, and cheaper the
faster the machine. Longer Smoothing is smoother but trails behind moving
clouds; longer Refresh is cheaper but they move in steps. The shape
noise is read through a slow wobble, so its repeats never line up into
rows of identical clouds. The screen then only looks
each pixel's direction up in the dome, unless the land is nearer. The
water still mirrors the flat sheet. Off, the flat clouds cost next to
nothing, for weak devices.


The northern lights (`aurora.c`, `data/shaders/aurora.glsl`): curtains of
light over the north (away from the noon sun), in a layer from 95 to 300 km
up. The sky's shader follows each ray up through the layer, over the round
earth (towards the horizon it meets it far away), in 24 steps of height; at
each the curtains are two bands running east to west, wavy, their waves
rolling, made of vertical rays that shimmer; bright at their sharp lower
edge, green, going red and violet higher up, a pink fringe at the bottom.
The water mirrors them; they add a faint green to the light from the sky.
The game says when (`aurora_set`; the World window's Aurora, the console's
`aurora on|off [strength]`); they show only in the dark (the sun 4 to 12
degrees under the horizon fades them in), and stay until put out.
`render.aurora_strength` (Sky & light) scales them.

Rain (`rain.c`, `data/shaders/rain*.vert|frag`): the simulation's rain
(`water.raining`: R, the Brushes' or the World window's Rain, the console's
`rain on|off [intensity]`) is also seen. The drops are thin streaks in a box
of 60 by 24 meters around the eye, slanted by the clouds' wind, stopping at
the ground or the water; nothing is simulated, where each drop is follows
from its number and the clock, and the box wraps around the eye. Where they
land, small rings spread on the ground and the water (splashes), and the
water's surface gets rings of its own (`water.frag`). While it rains the
sun and the sky's light dim, the haze thickens and the sky goes gray;
objects darken and shine as they get wet (the ground's wetness is the
simulation's moisture) and dry slowly afterwards. `render.rain_intensity`
(how hard: the number of drops and all the rest), `rain_streak_meters` and
`rain_splashes` are in Sky & light.

In the snow's months (`[render] snow_first_month` .. `snow_last_month`)
the rain falls as snow (`snow.vert|frag`): slow flakes, two or three centimeters
across, each falling at its own speed, swaying from side to side and
drifting with the wind more than a drop does, in a box of 44 m around the
eye; no splashes, no rings on the water, no wetting, no rainbow. Sleet (a
mix) for a week or so as the snow's months begin, rain again as they end.
The water the simulation's rain brings comes all the same (it is a game).

The rainbow (`rainbow.frag`): 42 degrees around the point opposite the sun,
red outside (each color its wavelength's, as bright as the eye sees it:
yellow and orange the brightest), a fainter secondary at 51 degrees with its colors reversed,
the sky brighter inside the first and darker between the two (Alexander's
band). It hangs 450 m off, in the rain: drawn over the sky and the land
farther than that (its depth is that of a point that far along the ray),
never over anything near. So it shows only with the sun above the horizon
and under 42 degrees, and never at night. The game says when
(`rainbow_set`; the World window's Rainbow, the console's
`rainbow on|off [strength]`); `render.rainbow_strength` scales it.

### Frozen water

Once the world looks right, the water can stop: F7, or by itself after
`freeze_after_seconds` (30 s) without the player touching terrain or water.
Then nothing is simulated: no water, no erosion, no soaking or evaporation,
and springs stop pouring. The water keeps the flow it had, so it is still
drawn flowing (the ripples and foam drift with the current) and floating
things still drift on it; the physics keeps running. Any tool that changes
terrain or water, a spring, rain, a new or loaded map, or F7 again starts
the water where it left off. Things falling into the water do not: only the
player does. The whole map stops and starts at once, so there are no seams
between frozen and running water. 0 for `freeze_after_seconds` freezes only
with F7. Space pauses everything, the physics too.

Water thinner than about a millimeter clings to the ground as a film: it does
not flow and slowly evaporates, so the map dries out after rain instead of
trickling forever. A fake surface tension does the same for full ponds: water
only spills onto dry ground when it stands a few millimeters above it, so a
pond filled to the brim stops draining over its lip instead of leaking slower
and slower forever. The lip (`lip_millimeters`, 20 mm) does the same where the
water below the lip is already wet: deep water spills into thin water only when
it stands that much above it, so a pond a little over its lip stops instead of
draining for hours through a trickle.

This is a game, not a hydrology tool, so **thin water** (up to
`thin_water_millimeters`, 3 cm) is helped along, each part with its own switch
in `[water]`:

* **Superfluid** (`thin_water_superfluid`): thin water flowing on over wet
  ground flows as if it were 3 cm deep, without the extra friction real
  shallow water has, so a trickle takes seconds instead of half an hour. Into
  dry ground it stays real, so a flood's front is the real, thick front and
  not a film racing ahead. Its speed (for erosion, the physics and sleeping)
  is measured as if it were 3 cm deep too, or a fast film would hit like
  rapids.
* **Drying** (`thin_water_evaporation_millimeters_per_second`): thin water
  left behind in still tiles, with no deeper water next to it, evaporates
  fast (not at lake shores).
* **Mud settling** (`still_water_speed_meters_per_second`,
  `still_water_deposition_per_second`): mud in water slower than that drops
  out within a second or so instead of keeping its tiles busy for minutes.

Lakes come to rest for good thanks to two more rules:

* **Static friction** (`still_level_millimeters`, 5 mm): water that just came
  into a cell through a pipe does not go back out through it for less level
  difference than that. A lake's rocking is water going back and forth, so
  it stops once the waves are smaller; flow in one direction is never held,
  so tilted water still levels out and rivers on gentle slopes flow.
* **Shore damping** (`shore_damping_depth_meters`, `shore_damping_per_second`):
  slow water gets braked harder the shallower it is near the shore, so waves
  die on the beach instead of bouncing back. Fast water is not touched.

Bodies ignore water shallower than `water_minimum_meters` (3 cm, in
`[physics]`), feel the water surface interpolated between cells, never see a
flow faster than 10 m/s, and floating ones have their bobbing braked
(`float_damping_per_second`).

The status line shows where the time goes, in milliseconds per frame: the
whole frame, drawing (CPU side, and the graphics card when it can be
measured), water, erosion and physics. Next to the average fps it shows the
minimum (the slowest frame of the last half second), since an average hides
stutters. Any frame much longer than the usual one is logged as a
`Stutter:` line with where its time went (update, poll, water, erosion,
wetting, physics, particles, water end, copy wait, acquire, navigation,
terrain upload, shadows, render, present) and what happened in it (steps,
substeps, evaporations, bank collapses, tiles copied back, chunk uploads,
physics rebuilds, shadow redraws). "acquire" is waiting for the screen
(vsync) or for a busy graphics card. Vertical sync applies at once, without
Apply.

The surface mirrors what is around it (Graphics → Water → Reflections,
`water_reflections`, on unless set). Before the water is drawn, the scene so
far (its color and its depth) is copied, and each reflected ray, bent by the
ripples, is marched over that copy in steps that grow as it goes (screen
space reflections): the shore, the hills, the trees and the people
standing by show upside down in it. Where the ray leaves the screen, goes
behind the eye or finds nothing, the sky takes over, with the volumetric
clouds looked up in their dome (the same one the sky shows, from the
eye), so the clouds drift over the lakes too. The mirror fades into the sky
near the screen's edges and at the end of the march's reach, and it is
strongest at grazing angles (Fresnel): looking straight down at the water
there is almost nothing to mirror, and the march is skipped. Off, the water
mirrors the sky alone, as before. The march follows only a third of the
ripples' bend (with all of it, at the water's level neighboring rays go
every which way and the shore breaks into stripes); a ray the ripples
bend down into the water bounces up again, as it would off the next wave;
and a hit counts only where the ray went through a surface in that step,
not behind a blade of grass. What is not on screen cannot be mirrored:
a tree behind the camera, or the far side of a hill. See
`data/shaders/water.frag`.

On request the marching is done for a quarter of the pixels
(`water_reflections_half_size`, Graphics → Water → Reflections at half
size): the water is drawn with the sky in the reflection's place, writing
beside its color each pixel's reflected ray and how much of the pixel its
reflection is; a compute pass marches, for each 2 x 2 pixels, the one that
mirrors the most; a last pass adds what was found in the sky's place, each
pixel taking the nearest marched texels whose distance matches its own (a
far lake's reflection does not bleed onto the near shore). Faster, a little
softer (off by default: the gain is small and the softness shows); off,
every pixel marches its own. The march itself: 32 steps
growing by 16% each, then 4 halvings to the surface.

Under water, the camera sees the world the way a diver does: everything
fades into the water's color with distance (shallow color near the surface,
deep color and darker further down, murky brown in muddy water), sunlight
focused by the ripples plays over the ground (caustics), and the surface
seen from below shows the sky only straight above (Snell's window, wobbling
with the ripples) and mirrors the water elsewhere. How far you see in clear
water (`underwater_visibility_meters`, mud cuts it down a lot) and how bright
the water around the camera is (`underwater_brightness`) are in Graphics →
Water. See `data/shaders/underwater.glsl`.

## Baked maps

An active map carries everything the simulation needs: per cell the
ground's layers (height, bedrock, silt, soil), the water's depth, its four
pipes, the mud in it, the ground's moisture, the walls: some 42 bytes on
the CPU and as much again on the GPU. A map you play on does not need any
of it once the water has found its way: a baked map keeps only what is
drawn and stood on, as it was when it was baked, and nothing moves.

* **What it keeps.** The heights in 16 bits (in steps of the map's height
  range / 65535: a few tenths of a millimeter on a valley, about 5 mm on
  300 m of mountains), the material map in two bytes (base and cover
  from a palette of up to 15 materials per map, the overlay, the shelter
  in 7 bits), and only in the 64 x 64 tiles with water, or with what the
  water left on the ground: the water's depth in the same steps as the
  heights (a lake's level stays flat), its flow as the water's textures
  showed it (velocity, mud, the busiest pipe's speed: a byte each), and
  the silt, the wetness and the bare bedrock the ground showed. The soils
  follow from the material shown (rock, sand, clay, else earth).
* **What it does not.** No water simulation, no erosion, no terrain or
  water tools, springs or brush (they say so when tried). The objects, the
  physics, the actors, the ice, the marks the water left, the weather,
  everything else, as on an active map; trees can be planted and cleared,
  layouts placed (their flattening is not followed by the water).
* **Memory.** On the CPU about 9 bytes a cell (the heights still as 32 bit
  integers, the soil, the material map) plus 4 for the water's depth, and
  7 more a cell in the tiles kept; on the GPU only the water's two
  textures (16 bytes a cell) instead of the simulation's buffers and
  textures (about 55). A 4096 map: roughly 220-340 MB of RAM and 270 MB of
  video memory instead of 700 and 880.
* **The file.** The same zip archive as an active map, `kind = baked` in
  its `map.ini`, every layer an entry of its own so they unpack side by
  side; a 1024 map with a flooded valley is 2 MB instead of 5 and loads in
  half the time (the details are in `map_file.h`).
* **The marks the water leaves** (see below) are not kept on a baked map:
  its water never goes down, so there is nothing to remember. What lies
  under its water has always lain there: the algae and the moss are grown
  in full, from the depth alone, with no texture and no pass for them.
* **Starting with one** (`--load NAME`): the world is made at its size and
  of its kind, empty; no land is generated, nothing planted, no paths
  worked out for a map that would be thrown away. Paths are found region
  by region as actors go (after any load).
* **Loading it fast.** Its big layers are cut into parts of 2 MB and its
  heights predicted in bands of 256 rows, so all of it unpacks and
  decodes on every core at once; its terrain's textures go up a row of
  chunks at a time with the normals made on the GPU (`terrain_normals.comp`);
  only the water's tiles with water go up (the rest cleared on the GPU);
  the physics makes the bodies within 96 m of the player at once and the
  rest 2 ms a step. The log tells each part's time (and the first three
  frames after a load, step by step).
* **Its look.** The same shaders draw both: on a baked map the terrain's
  textures are made from what was kept (once, at the load) and
  the water's textures are filled from it, so both kinds look the same
  (the foam and the ripples of running water are drawn from the kept
  flow, they still move).

Make maps active, bake them to play them: bake again after changing the
active one.

## Waterfalls

Where the water falls (a crater's lip, a cliff) it plays three particle
effects of `data/particles.ini`: `waterfall_curtain` along the lip,
`waterfall_mist` and `waterfall_splash` where it lands (`engine/waterfalls.h`).
A fall is where the water's surface drops more than 1.5 cells' widths from
one cell to the next downstream after it did not: its lip; the way down is
followed (the steepest, while it slopes) to where it lands; it counts from
2 m high and 5 liters a second. A lip further down another fall's way
(a ledge on its face the water pours over) is no fall of its own. Lips of
the same fall within 2 m make a
segment, whose effects are scaled by its width, its water and its height:
the curtain is pushed out gently (at most a meter a second: it follows
the face) and lives as long as sliding down it takes (and a little more, to
spread on the pool below: `collide = slide`, particles that slide down what
they land on), the mist grows with the height.

An active map looks for them every 3 seconds, within 400 m of the player;
a baked map has them in its file, found when it was baked. Those within
250 m play. The ice holds them still in the winter. The emitters can take a
push (meters a second, added to their particles' speed), a rate and a life
scale from what plays them (`particles_emitter_t`).

## Golden and blue hour

With the day cycle (`engine/daylight.c`): in the sun's last (and first)
degrees above the horizon, up to about 8, the sunlight goes a deep gold,
and the sky's light in the shadows a little cooler against it (the golden
hour); with the sun some 1 to 9 degrees under it, before the night's dark
blue, all the sky a deep, saturated blue (the warm glow on the sun's side
gone too) and its light stronger, shown a little brighter (the blue hour).
Not every dusk has one: `render.blue_hour_on` (off: the dusky twilight with
its warm glow, as before). `render.golden_hour` and `render.blue_hour`
(Sky, live; 0: none, 1: as made, up to 2: longer and stronger).

## Gusts on the meadow

When the wind blows, its gusts roll over the grass as a paler wave: the
blades a gust bows catch the light on their sides (`grass.vert`;
`render.grass_gust_sheen`, Objects > Gust waves, live), so the gusts can be
seen travelling over a field (`plant_wind_gust_meters` apart). The ground's grass far
off (the terrain's material, past the blades) takes the same waves.

## Sky occlusion

How much of the sky each cell of the ground sees, worked out once over the
whole map, a value a cell (`engine/sky_occlusion.h`): the ground and the grass
take that much of the sky's light (the sun keeps its shadows). From
`sky_occlusion_directions` directions spread evenly over the sky (from the
zenith down to `sky_occlusion_lowest_degrees` above the horizon) the
terrain and every object are drawn into a depth map, as the sun's far
shadow map is (texels of `sky_occlusion_texel_meters`, at most 8192 over
the map); each cell adds how visible it is from there, weighted by how
much that direction faces its ground, and at the end it is divided by
what it would add with nothing in the way. Crater floors, narrow valleys,
the ground under trees and round rocks get darker in the shade.

It is worked out a few directions a frame (`sky_occlusion_directions_per_frame`):
when a map comes without it (a new one, an old file), with U, or with
"Work it out" in Settings > Render (the old one is drawn until the new one
is done). It is kept with the map (both kinds: `sky_occlusion.bin`, a byte
a cell, each less the one before it in its row). What moves afterwards
leaves its shade until it is worked out again. `sky_occlusion_intensity`
(live) says how much of it shows; `sky_occlusion = false` turns it off.
It is worked out in the air too: four layers that follow the ground, 2, 6,
15 and 35 m above it, a texel about every meter (at most 4096 over the
map), one under the other in a texture, weighted by how high each
direction is (what lights a thing there from the sky); `sky_occlusion_layers.bin`
in the map, the same way. Things on the ground (rocks, trunks, crowns,
houses, actors, their impostors far off: `data/shaders/sky_occlusion.glsl`)
take the value at their height above the ground, between the ground's and
the layers'; above the last, the last: the foot of a rock, the trunks in a
dense wood, an actor under the crowns, a tree on a crater's floor (all the
way up: the crater's walls still hide the sky) get darker. A map saved
without the layers works it all out again.

It also darkens the sky the water mirrors and the sky's light on it (at the
water's surface: the layers, its depth above the ground), and the sky's
share of the lit particles' light (smoke, mist, spray; the glowing ones,
fireflies and sparks, keep theirs). The volumetric fog's sky light too. Bark and
weathered stone grow more moss where the sky is hidden (`MOSS_IN_SHADE`).

The light the land round sends back (one bounce of the sun's): the bake
also keeps, per texel of the ground (about a meter, at most 3072 over the
map; `sky_occlusion_bounce.bin`), which way and how far what hides its sky
is, on the whole. Drawn, what hides the sky lights the point as far as the
sun is on it there (the far shadow map) and it faces the sun the way it
faces back, in the land's color on the whole (`SKY_BOUNCE_ALBEDO`,
`sky_occlusion.glsl`): a crater's shady side gets the warm light of its
sunlit wall, a cliff's foot its face's; a crown overhead shows its
underside, out of the sun, and sends none. `render.bounce_light` (Bounce
light, live) says how much. One way per texel: in a narrow valley lit on
both sides it is their mean. The grass, the plants' leaves and the
impostors work both out per vertex (`sky_vertex.glsl`: a blade, a card,
an impostor's middle; they are drawn over and over on the same pixels and
the light changes slowly); the terrain and the solid objects per pixel,
once each.

## How the ground reacts

Where it is drawn from: the ground the simulation holds on the GPU. Each
frame `water_ground.comp` makes the terrain's textures (heights, normals,
surface: silt, bare rock, water on it) for the tiles whose ground or water
changed, there, without the CPU (`render.terrain_from_gpu`, Graphics →
Water → Terrain from the GPU; off, the CPU makes them from its copy, as
before, to compare). The silt and the bare rock shown follow the ground in
a second and a half, so a film of silt laid and washed away again and
again under running water does not flicker. The CPU's copy of the ground
and of the water (for the physics, the objects standing on the ground, the
paths, the shadows' redraws, the ground's moisture) comes back a tile at a
time, at most every quarter second (the ground) or tenth of a second (the
water; a twentieth near the camera); the moisture stays on the CPU and goes
up as a byte a cell for the surface's wetness.

Each cell is a column: **bedrock** at the bottom (never erodes, holds no
water), the cell's **soil** on top of it (deep in flat valleys, thin on steep
slopes), and **silt** on top, which is whatever the water dropped there.

* **Moisture.** Ground under water or in the rain gets wet until the soil
  column is saturated (sand fast, clay slowly); where nothing wets it, it
  dries out again. Wet ground looks darker. The water itself stays: the
  moisture is a number for the farms and the look, not water taken away.
* **Erosion.** Flow faster than a soil's critical speed picks material up,
  until the water carries as much as its speed and depth allow. Under
  flowing water the ground is soaked, so it gives way at the wet critical
  speed. Silt goes first, then the soil, never the
  bedrock; soil worn down to the bedrock shows bare rock. Water carrying
  sediment turns brown.
* **Trickles do not dig.** Erosion power grows with the depth: nothing below
  half of `erosion_minimum_depth_millimeters` (20 mm), full from it up. A
  lake seeping over its lip does not cut itself an outlet; one really
  overflowing does.
* **Deposition.** Water carrying more than it can (because it slowed down or
  ran thin) drops the excess as silt.
* **Bank collapse.** Banks with water at their foot (or on them) steeper
  than the soil's wet slope slide into the water. This is what really breaks a dam: the overflow cuts a notch, the
  sides of the notch slump in, the notch gets wider and deeper, more water
  goes through. Silt (mud the water dropped) is loose: it slumps to its
  slope even when dry.

* **Smoothing.** Under water, a single cell sticking out above (or dipping
  below) both its neighbors on a line, the sawtooth erosion carves under
  cliffs, is evened out with them (`erosion_smoothing` in `soils.ini`). Only
  loose material moves, never bedrock.

All of it only runs where water flows or carries sediment. The fine shadow
map around the camera's target is redrawn when the ground in it has changed
by `shadow_erosion_meters`; the far map ignores erosion (it does not show
centimeters), only tools and stamps redraw it. Objects carried by the water
only redraw the fine map; in the far one a log is a pixel of a soft shadow.
Objects standing on the ground are set 2 cm into it, so erosion does not
uncover their feet; once the ground under a chunk wore away more than that,
its objects are put back on it (a few thousand a frame).

## What the ground shows (the material map)

The CPU decides **where** and **what**, the GPU **how much**. Per cell the
terrain keeps four bytes (`material_map.h`, saved in the map as
`materials.bin`; an older map makes them again when it loads):

* **base**: the material the ground itself shows: grass, dirt, rock, sand
  or clay. Worked out once (`material_map.c`, in parallel: about a second
  of one core for a 1024 map) by the rules the shader used to apply to every
  pixel every frame: the soils (blurred `soil_blend_meters`, their borders
  moved about by noise), the slope (the wide "material" normal), how the
  slope faces the noon sun (`sun_azimuth_degrees`: not the sun of the
  hour, so grass and dirt no longer creep as the day goes by), the noises,
  cliffs as rock; the patches where materials meet are in it already. Done
  again only where tools, stamps or layouts change the shape or the soils
  (as far round as the rules look); erosion does not change it.
* **cover**: what lies over the base where something put it: fallen
  needles, leaves, trampled earth. Objects leave it (objects.ini `cover`,
  below; whatever stands in a layout tramples the ground round it), when
  the map is populated, when they are planted or placed. A flood takes it
  away (water deeper than 5 cm), and silt buries it. Moving objects never
  leave any: a log carried off does not drag its cover along.
* **overlay**: what settles on all of it in amounts: the petals a blooming
  tree drops (`cover = petals`), snow anywhere.
* **shelter**: how covered from above the cell is (0 open sky .. a roof)
  by what stands there for good (objects.ini `shelter`): less snow there.

The terrain shader (and the grass, `terrain_materials.glsl`) reads the four
cells round a pixel, each by how near (a cell's cover over its base), and
breaks the borders between them into patches as before; then what the
water did comes over it every frame from the GPU's surface texture (bare
bedrock, silt), then the snow by its line (high up, lower on shady
slopes), then the overlays. Priority, highest first: snow, silt, the bare
bedrock, cover, base. F2's second debug view shows the materials in flat
colors. `render.material_map = false`
works them out per pixel the old way (no cover, no overlays), to compare.

**Snow and petals** (`ground_overlay.c`, `ground_overlay.comp`): one
RGBA16F texel per cell holds how much snow and how many petals lie there,
worked out on the GPU a few times a second. Snow comes only from the
snowfall (the rain, R, in the snow's months, as hard as Sky → Rain says):
at full strength it covers the open ground in a minute of game time.
Less under shelter, and never more than the open sky there (under a crown
of 0.7, a third at most, round its edge), none on faces steeper than some
40 degrees or on water. The winter only keeps it from melting; after it
it melts in four minutes (faster on slopes facing the sun), at once under
water. Through the winter it melts too, at a seventh of that (some mild
days): it snows, melts and snows again. A map started in the winter, or a
jump in the month, starts with a layer, half of what the winter would
hold. Petals fall where the map
has them through `bloom_first_month` .. `bloom_last_month` (April, May) and
go after it, or with the water. Snow lies on things too
(`objects_t.snow_amount`: five minutes to load them fully at full
snowfall, melting as on the ground): the bark and the sagging
branches, the props, and the models (`object.frag`: on what faces up, in
patches, matte; not on what is cut out, leaves and stalks; nor on what lies
under water, a rock in the river: `ground_water.glsl` looks the water up
where it is). Snowfall does
not wet things the way rain does. An evergreen's winter pictures (a conifer: its foliage never falls;
 its atlas' months, tools/potop_plant) go by
the snow too, through the snow's months and the one either side of them
(November to March with the snow from December to February): four of
them, the least snow first (the month after the snow's last, the month
before its first, its first, the middle of it: March, November, December,
January); with none on things the plant shows its green. Each card climbs
the steps as the snow grows and comes down them as it melts, a little
ahead of the others or behind, so a tree whitens and greens again branch
by branch, nothing blended. Outside those months its pictures go by the
month as before. A conifer laden with snow leans in the wind about half as far,
and its needles hardly shake. The grass left standing in the snow (under the trees) fades to
straw, frosted at its tips. The snow falls nearly straight down, drifting
little with the wind. An overlay shows where its **mask** is below its amount:
hollows first, then more and more, not a blend. The mask is the alpha of
the overlay material's normal map: `NAME_mask.png` if there is one
(`potop_texture petals` and `snow` make one), else its height. The grass
hides under the snow as it deepens.

objects.ini, per type (none by default):

```
cover = needles          ; needles, leaves, trampled, petals, none
cover_meters = 0         ; how far round (at size 1); 0: under its crown (the model's width)
shelter = 0              ; how much it keeps off what falls (0..1)
shelter_meters = 0       ; how far round; 0: as far as its cover
months = 4, 10           ; there from the start of April to the end of October (all: all year)
```

Like the wind, these do not carry over from one `[defaults]` to the next:
each group starts with none of them.

`months`: out of its season a plant is not drawn at all (nor its shadow);
each one comes on its own day in the first half month and goes on its own
day in the last, so a meadow fills and thins over a fortnight, none of them
shrinking or fading. Only the month decides, not the snow: a thaw in
February brings no dandelions back. They stay on the map (saved, picked, flattened by the water) whether they show
or not.

settings.ini `[render]`: `material_map = true`, `ground_overlay = true`
(snow and petals), `bloom_first_month = 4`, `bloom_last_month = 5`; the
first two are in Settings → Ground → Material map too, live. Each has its
own shaders (`terrain_map.frag`, `grass_map.comp`; off: `terrain.frag`,
`grass.comp`, the old way, from the same `terrain_body.glsl` and
`grass_body.glsl`), so the one switched off costs nothing. The snow and
petals' pass rests while nothing falls and nothing lies (summer: some
minutes after the last snow melted it clears what is left and stops).

materials.ini: the new materials are `[needles]`, `[leaves]`,
`[trampled]` (covers) and `[petals]` (an overlay); their textures default
to `needles_01`, `leaves_01`, `trampled_01`, `petals_01` (make_textures.sh
makes them), else the procedural fallback.

## What the water leaves on things

The objects' shaders (rocks, houses, props, the plants' bark and foliage)
know where the water stands and where it stood (`ground_water.glsl`; the
memory per 2 x 2 cells on the GPU, `water_marks.c`, `water_marks.comp`,
a few times a second while there is water, resting once it is long gone):

* **Under water**: darker and wet; the sunlight plays over it (caustics,
  fading with the depth); where a thing meets the surface a thin bright
  line wobbles with the ripples (not on foliage). No snow lies there.
* **The flood's mark**: what stood under water half a minute or so keeps,
  once the water goes, a film of mud up to where it stood a while (a passing
  wave leaves next to nothing), dark brown while wet, a pale dusty grey-beige
  once dry, a darker line at its edge, gone in some fifteen minutes of game
  time; foliage is only dulled by it. The mark belongs to the spot, not the
  thing: what floats, or was ever moved by the physics, takes none of it
  (only the water there now).
* **Drying**: what was under water stays wet (darker, shiny) for four
  minutes after.
* **Algae**: what stays under water grows a green-brown film over a quarter
  of an hour, in patches; out of the water it dries pale and fades.

What is whose: solid things take all of it, bark all but the shine,
foliage only the mud and the sunlight under water.

**Moss under water** on the rocks (the weathered props, `prop_body.glsl`):
where the algae would grow it grows instead (in patches, not under
overhangs; without the marks' memory, wherever the water is now), green
in the water, pale once dry. Painted on, but seen into: a few steps along
the view through its strands (parallax), which bump the light. Near the
eye it also stands up in shells (`prop_shell.vert`, `prop_shell.frag`):
the rock drawn again a few times, lifted a little off its surface each
time, its strands cut out of every layer, their tips swaying. Only the
props at their finest level of detail, within `moss_shell_distance_meters`;
a draw per layer for each of their models.

settings.ini `[render]`: `moss_shells = 8` (0: painted on alone),
`moss_shell_meters = 0.02` (how tall it stands),
`moss_shell_distance_meters = 12`.

settings.ini `[render]`: `water_marks = true` (false: no memory of the
flood, its texture let go and its pass not run; the water there now still
wets, lights and lines what it touches).

## Ice on the water

Through the winter (the snow's months) still water freezes (`water_ice.c`,
`water_ice.comp`: a texel a cell on the GPU, a few times a second, resting
once the winter is over and all of it has melted): first at the shore and
where it is shallow, then out over the water from the ice there, a ragged
front; its thickness grows with the square root of the time (2 cm in a
minute of deep winter, some 15 cm in an hour). Water running faster than
about 0.15 m/s freezes slower, past 0.5 m/s not at all, and wears what ice
it has away. After the winter it melts. Water at its edge standing 12 cm
higher than it (a flood coming) breaks it.

Where it is 3 cm thick or more the water under it holds still: the
simulation's pipes in and out of those cells are shut (`water_fluxes.comp`);
a frozen lake goes to sleep, what floats on it stays where it is. Cold
enough (the winter from `WATER_ICE_HOLDS_WINTER`, 0.35, on) all of the
water holds still: the simulation does not step (nor erode), running water
and falls too, and freezes over everywhere; F7 and the tools do not start
it, the winter's end does (the status line: "water ICED"). Then the ice
bears: the walker and the actors stand on its surface (no wading, no
swimming), the navigation takes it for dry ground, and the physics' ground
is its surface where there is water (loose things lie on it; the water
under it pushes nothing; what was under the water when it froze is held in
the ice until it thaws). The surface walked on is the water's level
smoothed over its neighbors: the ripples it froze in are not felt. The Diagnostics' "ice:" line says whether all of
the water is frozen still.

The water renderer draws it over the water: thin, black and glassy, a
sharper mirror; thicker, clear blue green, frosted here and there (a dim,
blurred mirror there, a broad glint), small bubbles in it, cracks through
it; no ripples, rain rings or foam through it; it fades out at the shore as
the water does. (Snow on the ice: not yet.)

settings.ini `[render]`: `water_ice = true` (false: never; its texture let
go). Diagnostics: the "ice:" line.

## Known limitations

* Terrain shape is tuned for the default 1024 map; smaller maps keep the same
  number of features and come out steeper.
* The water is a single layer: no water under water, no breaking waves.
* The procedural fallback textures are placeholders.
* The dam stamp's preview shows the dragged line only, not the extension
  into the banks.
* Groundwater does not flow: moisture stays where it is until it dries.
* Small objects (up to about 1.2 m) buried in silt are gone for good: out of
  the physics, the navigation and the map.
* The interface is for desktop (mouse and keyboard); touch will need its own.
* Changing the anisotropy does not reach the objects' textures until the
  next start.

## Third party

* `third_party/box3d/`: Box3D, MIT, by Erin Catto (https://github.com/erincatto/box3d);
  the commit is in `VERSION.txt`. It is alpha software: updated only on purpose.
* `third_party/nuklear.h`: Nuklear 4.13.3, public domain / MIT
  (https://github.com/Immediate-Mode-UI/Nuklear).
* `third_party/cgltf.h`: cgltf 1.14, MIT, by Johannes Kuhlmann
  (https://github.com/jkuhlmann/cgltf); only the tools use it.
* `third_party/miniz.c`, `miniz.h`: miniz 3.0.2, MIT (see `miniz_LICENSE`),
  by Rich Geldreich and others (https://github.com/richgel999/miniz).
* `third_party/bc7enc/`: bc7enc (2020), MIT / public domain, by Rich Geldreich
  (https://github.com/richgel999/bc7enc); only the tools use it (the plant
  atlases).
* `third_party/bcdec/bcdec.h`: bcdec 0.985, MIT / public domain, by Sergii
  Kudlai (https://github.com/iOrange/bcdec): the plant atlases decoded where
  the GPU has no BC7.
* `data/ui/fonts/DejaVuSans.ttf`: DejaVu Sans, free license (see
  `DejaVu_LICENSE.txt` next to it).

* `third_party/stb_image.h`, `stb_image_write.h`: public domain / MIT, by Sean
  Barrett (https://github.com/nothings/stb).
