# Building

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
