# Baked maps

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
