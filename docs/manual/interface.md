# Interface

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
* **Icon bar** (top right): settings, save, open, new (projects), pause, slower,
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
  | `project list \| new <name> [seed] \| open <name> \| save [name] \| bake \| import <map> <name>` | map projects (the editor's; see **Projects** below) |
  | `screenshot` | as F12 |
  | `clear` / `quit` | clears the log / leaves |

  The same commands are meant to be what the game's API speaks later.
* **World** (top bar): what is shown of the world. Objects: off, they
  are not drawn, nor their shadows and lights; they stay and go on (the
  physics, the water, the paths, saving), only unseen. Fireflies: how many
  on dark nights (0: none).
* **Projects** (the editor): every map is made in a project, a folder of
  its own in the preference directory's `projects` (on Linux
  `~/.local/share/potop/potop/projects/NAME/`): `project.ini` (its name,
  seed, size, when it was made), `map.potop` (the map being made),
  `baked.potop` (the last bake) and later `sketch/` (the 2D sketch the
  terrain is made from). **New** makes one (a name, a seed; the size as
  Settings, New map say); **Save** saves the project open (F5 too) or
  **Bake (for play)**: into the project, and a copy into the saves as
  `NAME.baked` for the game; or saves the map as a new project. **Open**
  opens one (or deletes it with all in it), and lists the maps saved
  before projects (or by the game): **Bring it in as a project**, or
  **Just load it** outside any project. F9 opens the project open as last
  saved. N (a new map at once) and a map just loaded leave the project.
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
