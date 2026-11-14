# Ice on the water

Through the winter (the snow's months) still water freezes (`water_ice.c`,
`water_ice.comp`: a texel a cell on the GPU, a few times a second, resting
once the winter is over and all of it has melted): first at the shore and
where it is shallow, then out over the water from the ice there, a ragged
front; its thickness grows with the square root of the time (2 cm in a
minute of deep winter, some 15 cm in an hour). Water running faster than
about 0.15 m/s freezes slower, past 0.5 m/s not at all, and wears what ice
it has away. After the winter it melts. Water at its edge standing 12 cm
higher than it (a flood coming) breaks it.

Where it is 3 cm thick or more the water under it holds still: the
simulation's pipes in and out of those cells are shut (`water_fluxes.comp`);
a frozen lake goes to sleep, what floats on it stays where it is. Cold
enough (the winter from `WATER_ICE_HOLDS_WINTER`, 0.35, on) all of the
water holds still: the simulation does not step (nor erode), running water
and falls too, and freezes over everywhere; F7 and the tools do not start
it, the winter's end does (the status line: "water ICED"). Then the ice
bears: the walker and the actors stand on its surface (no wading, no
swimming), the navigation takes it for dry ground, and the physics' ground
is its surface where there is water (loose things lie on it; the water
under it pushes nothing; what was under the water when it froze is held in
the ice until it thaws). The surface walked on is the water's level
smoothed over its neighbors: the ripples it froze in are not felt. The Diagnostics' "ice:" line says whether all of
the water is frozen still.

The water renderer draws it over the water: thin, black and glassy, a
sharper mirror; thicker, clear blue green, frosted here and there (a dim,
blurred mirror there, a broad glint), small bubbles in it, cracks through
it; no ripples, rain rings or foam through it; it fades out at the shore as
the water does. (Snow on the ice: not yet.)

settings.ini `[render]`: `water_ice = true` (false: never; its texture let
go). Diagnostics: the "ice:" line.
