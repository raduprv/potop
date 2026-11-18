# Controls

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

In the game (`potop`), F4 shows the navigation mesh in the world, 30 m
round the eye: the rectangles a walker's body fits in, for one width of
walker at a time (press again for the next narrower one, then off; a
width shows once a walker of it has looked for a way). The ground's
rectangles are green and blue (tints only tell neighbors apart), objects'
tops yellow at the ground through red to purple at 3 m and more, with
their height over the ground written on them; white lines are the
openings from one rectangle to the next, pink ones edges that are only
jumped. The small things a way is led round (trunks, boulders: see
actors.md) have a ring at each end of their capsules, orange; soft ones
(a bucket) blue. What stands nearer hides it, as it would in the world.

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
