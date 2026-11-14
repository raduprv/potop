# Sky occlusion

How much of the sky each cell of the ground sees, worked out once over the
whole map, a value a cell (`engine/sky_occlusion.h`): the ground and the grass
take that much of the sky's light (the sun keeps its shadows). From
`sky_occlusion_directions` directions spread evenly over the sky (from the
zenith down to `sky_occlusion_lowest_degrees` above the horizon) the
terrain and every object are drawn into a depth map, as the sun's far
shadow map is (texels of `sky_occlusion_texel_meters`, at most 8192 over
the map); each cell adds how visible it is from there, weighted by how
much that direction faces its ground, and at the end it is divided by
what it would add with nothing in the way. Crater floors, narrow valleys,
the ground under trees and round rocks get darker in the shade.

It is worked out a few directions a frame (`sky_occlusion_directions_per_frame`):
when a map comes without it (a new one, an old file), with U, or with
"Work it out" in Settings > Render (the old one is drawn until the new one
is done). It is kept with the map (both kinds: `sky_occlusion.bin`, a byte
a cell, each less the one before it in its row). What moves afterwards
leaves its shade until it is worked out again. `sky_occlusion_intensity`
(live) says how much of it shows; `sky_occlusion = false` turns it off.
It is worked out in the air too: four layers that follow the ground, 2, 6,
15 and 35 m above it, a texel about every meter (at most 4096 over the
map), one under the other in a texture, weighted by how high each
direction is (what lights a thing there from the sky); `sky_occlusion_layers.bin`
in the map, the same way. Things on the ground (rocks, trunks, crowns,
houses, actors, their impostors far off: `data/shaders/sky_occlusion.glsl`)
take the value at their height above the ground, between the ground's and
the layers'; above the last, the last: the foot of a rock, the trunks in a
dense wood, an actor under the crowns, a tree on a crater's floor (all the
way up: the crater's walls still hide the sky) get darker. A map saved
without the layers works it all out again.

It also darkens the sky the water mirrors and the sky's light on it (at the
water's surface: the layers, its depth above the ground), and the sky's
share of the lit particles' light (smoke, mist, spray; the glowing ones,
fireflies and sparks, keep theirs). The volumetric fog's sky light too. Bark and
weathered stone grow more moss where the sky is hidden (`MOSS_IN_SHADE`).

The light the land round sends back (one bounce of the sun's): the bake
also keeps, per texel of the ground (about a meter, at most 3072 over the
map; `sky_occlusion_bounce.bin`), which way and how far what hides its sky
is, on the whole. Drawn, what hides the sky lights the point as far as the
sun is on it there (the far shadow map) and it faces the sun the way it
faces back, in the land's color on the whole (`SKY_BOUNCE_ALBEDO`,
`sky_occlusion.glsl`): a crater's shady side gets the warm light of its
sunlit wall, a cliff's foot its face's; a crown overhead shows its
underside, out of the sun, and sends none. `render.bounce_light` (Bounce
light, live) says how much. One way per texel: in a narrow valley lit on
both sides it is their mean. The grass, the plants' leaves and the
impostors work both out per vertex (`sky_vertex.glsl`: a blade, a card,
an impostor's middle; they are drawn over and over on the same pixels and
the light changes slowly); the terrain and the solid objects per pixel,
once each.
