# Command line

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
