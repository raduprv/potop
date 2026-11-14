# Compass, markers and the map

Markers (`engine/markers.h`) are places shown on the compass and the map:
`markers_add` (where, a color, a name), `markers_remove`, `markers_move`,
`markers_set_active` (the objective followed). The journal puts the
tracked quests' objectives (see Journal); the player puts waypoints of
their own on the map (one put is followed, no objective in focus then).

On foot the compass runs across the top: the letters and ticks turn with
the eye (north is -Z, the map's top), each marker a dot of its color (at
the edge with an arrow when it is out of view), the active one ringed with
how far it is.

**M** on foot opens the world map: an artist's picture if there is one
(`data/maps/NAME.png`, NAME the map's saved name without `.active` or
`.baked`, else `data/maps/world.png`; north up, covering the whole map),
else one made from the land (`engine/world_map.c`: the relief shaded from the
north-west, the ground's materials in muted colors, the water blue by its
depth, the woods darker, on a paper tone; made again when opened if the
map changed or the last one is half a minute old, about 70 ms for 1024²).
The markers with their names, the player as an arrow the way they look.
The wheel zooms (about the pointer), a drag moves it, a right click puts a
waypoint (followed at once), a right click on it takes it away.
