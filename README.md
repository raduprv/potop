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

## Getting started

Needs a C11 compiler, `make`, `pkg-config`, **SDL3** (3.2.30 or newer) and
**glslangValidator** for the shaders (`glslang-tools` on Debian based
systems); to run, a Vulkan driver.

```sh
make              # the engine, the game, the editor and the shaders
./potop           # the game
./potop_editor    # the editor
make tools        # the desktop tools (plants, props, houses, textures, converters)
```

The assets (textures, models) are not in the repository yet.

## Documentation

The manual is in [`docs/manual/`](docs/manual/README.md): building
(including SDL3 from source), controls, settings, the water, the sky, the
objects and their formats, the tools, the game side.

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

## License

MIT; see [`LICENSE`](LICENSE).
