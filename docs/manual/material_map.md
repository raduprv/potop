# What the ground shows (the material map)

The CPU decides **where** and **what**, the GPU **how much**. Per cell the
terrain keeps four bytes (`material_map.h`, saved in the map as
`materials.bin`; an older map makes them again when it loads):

* **base**: the material the ground itself shows: grass, dirt, rock, sand
  or clay. Worked out once (`material_map.c`, in parallel: about a second
  of one core for a 1024 map) by the rules the shader used to apply to every
  pixel every frame: the soils (blurred `soil_blend_meters`, their borders
  moved about by noise), the slope (the wide "material" normal), how the
  slope faces the noon sun (`sun_azimuth_degrees`: not the sun of the
  hour, so grass and dirt no longer creep as the day goes by), the noises,
  cliffs as rock; the patches where materials meet are in it already. Done
  again only where tools, stamps or layouts change the shape or the soils
  (as far round as the rules look); erosion does not change it.
* **cover**: what lies over the base where something put it: fallen
  needles, leaves, trampled earth. Objects leave it (objects.ini `cover`,
  below; whatever stands in a layout tramples the ground round it), when
  the map is populated, when they are planted or placed. A flood takes it
  away (water deeper than 5 cm), and silt buries it. Moving objects never
  leave any: a log carried off does not drag its cover along.
* **overlay**: what settles on all of it in amounts: the petals a blooming
  tree drops (`cover = petals`), snow anywhere.
* **shelter**: how covered from above the cell is (0 open sky .. a roof)
  by what stands there for good (objects.ini `shelter`): less snow there.

The terrain shader (and the grass, `terrain_materials.glsl`) reads the four
cells round a pixel, each by how near (a cell's cover over its base), and
breaks the borders between them into patches as before; then what the
water did comes over it every frame from the GPU's surface texture (bare
bedrock, silt), then the snow by its line (high up, lower on shady
slopes), then the overlays. Priority, highest first: snow, silt, the bare
bedrock, cover, base. F2's second debug view shows the materials in flat
colors. `render.material_map = false`
works them out per pixel the old way (no cover, no overlays), to compare.

**Snow and petals** (`ground_overlay.c`, `ground_overlay.comp`): one
RGBA16F texel per cell holds how much snow and how many petals lie there,
worked out on the GPU a few times a second. Snow comes only from the
snowfall (the rain, R, in the snow's months, as hard as Sky → Rain says):
at full strength it covers the open ground in a minute of game time.
Less under shelter, and never more than the open sky there (under a crown
of 0.7, a third at most, round its edge), none on faces steeper than some
40 degrees or on water. The winter only keeps it from melting; after it
it melts in four minutes (faster on slopes facing the sun), at once under
water. Through the winter it melts too, at a seventh of that (some mild
days): it snows, melts and snows again. A map started in the winter, or a
jump in the month, starts with a layer, half of what the winter would
hold. Petals fall where the map
has them through `bloom_first_month` .. `bloom_last_month` (April, May) and
go after it, or with the water. Snow lies on things too
(`objects_t.snow_amount`: five minutes to load them fully at full
snowfall, melting as on the ground): the bark and the sagging
branches, the props, and the models (`object.frag`: on what faces up, in
patches, matte; not on what is cut out, leaves and stalks; nor on what lies
under water, a rock in the river: `ground_water.glsl` looks the water up
where it is). Snowfall does
not wet things the way rain does. An evergreen's winter pictures (a conifer: its foliage never falls;
 its atlas' months, tools/potop_plant) go by
the snow too, through the snow's months and the one either side of them
(November to March with the snow from December to February): four of
them, the least snow first (the month after the snow's last, the month
before its first, its first, the middle of it: March, November, December,
January); with none on things the plant shows its green. Each card climbs
the steps as the snow grows and comes down them as it melts, a little
ahead of the others or behind, so a tree whitens and greens again branch
by branch, nothing blended. Outside those months its pictures go by the
month as before. A conifer laden with snow leans in the wind about half as far,
and its needles hardly shake. The grass left standing in the snow (under the trees) fades to
straw, frosted at its tips. The snow falls nearly straight down, drifting
little with the wind. An overlay shows where its **mask** is below its amount:
hollows first, then more and more, not a blend. The mask is the alpha of
the overlay material's normal map: `NAME_mask.png` if there is one
(`potop_texture petals` and `snow` make one), else its height. The grass
hides under the snow as it deepens.

objects.ini, per type (none by default):

```
cover = needles          ; needles, leaves, trampled, petals, none
cover_meters = 0         ; how far round (at size 1); 0: under its crown (the model's width)
shelter = 0              ; how much it keeps off what falls (0..1)
shelter_meters = 0       ; how far round; 0: as far as its cover
months = 4, 10           ; there from the start of April to the end of October (all: all year)
```

Like the wind, these do not carry over from one `[defaults]` to the next:
each group starts with none of them.

`months`: out of its season a plant is not drawn at all (nor its shadow);
each one comes on its own day in the first half month and goes on its own
day in the last, so a meadow fills and thins over a fortnight, none of them
shrinking or fading. Only the month decides, not the snow: a thaw in
February brings no dandelions back. They stay on the map (saved, picked, flattened by the water) whether they show
or not.

settings.ini `[render]`: `material_map = true`, `ground_overlay = true`
(snow and petals), `bloom_first_month = 4`, `bloom_last_month = 5`; the
first two are in Settings → Ground → Material map too, live. Each has its
own shaders (`terrain_map.frag`, `grass_map.comp`; off: `terrain.frag`,
`grass.comp`, the old way, from the same `terrain_body.glsl` and
`grass_body.glsl`), so the one switched off costs nothing. The snow and
petals' pass rests while nothing falls and nothing lies (summer: some
minutes after the last snow melted it clears what is left and stops).

materials.ini: the new materials are `[needles]`, `[leaves]`,
`[trampled]` (covers) and `[petals]` (an overlay); their textures default
to `needles_01`, `leaves_01`, `trampled_01`, `petals_01` (make_textures.sh
makes them), else the procedural fallback.
