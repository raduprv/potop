# How the water works

Every cell holds a column of water and is connected to its four neighbors by
"pipes". The flow through each pipe has inertia: a difference in water level
accelerates it, friction (stronger in shallow water) slows it down. That
inertia is what makes waves, overshoot and sloshing. A cell never sends more
water than it holds: if its four pipes together would, they are all scaled
down. Deep water makes waves fast, so the step is split into substeps when
needed to stay stable.

In the classic virtual pipes model every pipe has the same cross section
(a cell squared), so a film of a few millimeters on a slope races at tens of
meters per second. By default the pipes are sized by the water that can
actually pass between the two cells (from the higher water level down to the
higher ground), up to `pipe_depth_limit_meters` (2 cm by default): thin
films flow slowly, deeper water behaves exactly as before. (Sizing them by the full depth, which is the
shallow water equations, makes big waves pile up into sawtooth fronts: their
crests travel faster than their troughs, and the equations lack the
dispersion that keeps real short waves round.) M switches between the two.

Waves in this model travel at about sqrt(g * pipe depth), and the pipes are
never deeper than a cell, so deep lakes do not need extra substeps; the step
is split only if a wave could cross too much of a cell in one.

The water itself can never move more than a cell per substep (a cell cannot
give away more than it holds): 7.5 m/s at 0.25 m cells and 30 steps per
second. When flow at least a cell deep gets close to that, the next steps
are split into more substeps, so a breached dam or a waterfall is not held
back; it costs extra only while the water is that fast. What does set the
top speed of fast water is Manning's bed friction (`manning_roughness`,
the formula hydrologists use for rivers): it grows with the square of the
speed, so half a meter of water down a 1:3 slope tops out around 10 m/s
instead of accelerating for as long as the slope lasts. The plain linear
friction (`friction`) still handles slow water and lets lakes settle.

On screen the water level is blended a little with the neighboring cells,
which hides the one cell zigzags the simulation leaves on advancing fronts
and short choppy waves; the simulation itself is not touched.

The map is divided into 64x64 tiles: dry tiles are skipped, tiles whose water
has come to rest go to sleep, and tiles wake up when water flows in, a
neighbor's level changes against them, or a tool, spring or rain touches them.

Once asleep, a tile stays asleep unless something big happens: water pouring
into one of its cells faster than `asleep_wake_millimeters_per_second`, a
neighbor's level dropping `asleep_level_wake_millimeters` below its own (a
dam giving way), a tool, spring or rain. Smaller trickles running into it are
thrown away (counted with the evaporated water), so the calm parts of a lake
cannot keep waking each other over millimeters. A tile goes to sleep after
`sleep_after_substeps` substeps of still water.
