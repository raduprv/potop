# Waterfalls

Where the water falls (a crater's lip, a cliff) it plays three particle
effects of `data/particles.ini`: `waterfall_curtain` along the lip,
`waterfall_mist` and `waterfall_splash` where it lands (`engine/waterfalls.h`).
A fall is where the water's surface drops more than 1.5 cells' widths from
one cell to the next downstream after it did not: its lip; the way down is
followed (the steepest, while it slopes) to where it lands; it counts from
2 m high and 5 liters a second. A lip further down another fall's way
(a ledge on its face the water pours over) is no fall of its own. Lips of
the same fall within 2 m make a
segment, whose effects are scaled by its width, its water and its height:
the curtain is pushed out gently (at most a meter a second: it follows
the face) and lives as long as sliding down it takes (and a little more, to
spread on the pool below: `collide = slide`, particles that slide down what
they land on), the mist grows with the height.

An active map looks for them every 3 seconds, within 400 m of the player;
a baked map has them in its file, found when it was baked. Those within
250 m play. The ice holds them still in the winter. The emitters can take a
push (meters a second, added to their particles' speed), a rate and a life
scale from what plays them (`particles_emitter_t`).
