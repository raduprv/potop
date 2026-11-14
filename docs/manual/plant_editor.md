# The plant editor

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
