Plant recipes: what tools/plant_editor and tools/potop_plant grow the game's
plants from (see tools/plant_grow.h). Sources, as the pictures are: they are
not part of data/ and never ship with the game.

  tools/plant_editor plant_plans/cherry.plant         the plant in a window
  tools/potop_plant --recipe plant_plans/cherry.plant  the same, from the command line

Run both from the project's directory (paths in a recipe are as seen from
there; the bark must be under data/).
