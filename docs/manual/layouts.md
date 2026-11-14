# Layouts

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
