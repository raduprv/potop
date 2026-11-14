# Decals

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
