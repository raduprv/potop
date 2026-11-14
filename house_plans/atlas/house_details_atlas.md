# The houses' details atlas: what to paint

One square picture, 2048 by 2048 pixels, cut into cells (see house_details_template.png: each cell is framed
in red, with its number and name in its corner). Every cell is a separate small picture of one thing a medieval
house wears; the game maps a window light, a shutter, the end of a beam or a hanging sign onto its cell, so each
cell must be painted straight on (orthographic, seen square from the front), filling the whole cell edge to edge,
in flat, even, shadowless daylight (the game lights it; do not paint a sun's shadow or highlights), with nothing
of the surroundings (no wall round a window, no frame round a door leaf: only the thing itself).

Style: a realistic, hand painted, weathered look, muted colors, the kind of thing found on a half timbered house
in an old town: oak gone grey, iron gone rusty, lime wash, old glass. Nothing cartoonish, no outlines. The
pixel scale is the same in every cell: 256 pixels are half a meter (so a window light is 0.5 by 1 m, a door
leaf 1 by 2 m, a sign 1 by 0.5 m, a joist's end a quarter of a meter), and details should be sized accordingly.

Transparency: where a cell says so (lantern, horseshoe, signs, hook, flower box, sundial, the heart in the
shutter), the picture has a shape, and everything outside the shape is fully transparent (alpha 0); keep a
clean hard edge. Elsewhere the cell is fully opaque. The glass of the windows is painted as seen by day;
the game makes it glow at night by itself.

Deliver the picture as a PNG with alpha, 2048 by 2048, no compression artifacts, the cells exactly where the
template puts them (paint over a copy of the template, then remove its red frames and labels).

## The cells

| # | name | pixels (x, y, width, height) | meters | what to paint |
|---|---|---|---|---|
| 0 | window_leaded_diamond | 0, 0, 256, 512 | 0.50 x 1.00 | Window glass: diamond leaded lights (quarries about 10 cm across), old greenish glass, slightly uneven, lead cames dark grey. Fills the whole cell. |
| 1 | window_leaded_square | 256, 0, 256, 512 | 0.50 x 1.00 | Window glass: rectangular leaded panes (about 12 by 15 cm), same glass and lead as the first. |
| 2 | window_four_pane | 512, 0, 256, 512 | 0.50 x 1.00 | Window glass: a wooden cross of muntins (painted wood, 3 cm wide) with four clear panes, a little wavy. |
| 3 | window_roundels | 768, 0, 256, 512 | 0.50 x 1.00 | Window glass: crown glass roundels (bullseye discs about 11 cm) set in lead, the gaps between filled with small lead-set pieces. |
| 4 | window_dirty | 1024, 0, 256, 512 | 0.50 x 1.00 | Window glass: diamond leaded as the first, but grimy, a crack across one corner, cobwebs at the top, a quarry replaced by a newer clearer one. |
| 5 | window_boarded | 1280, 0, 256, 512 | 0.50 x 1.00 | Window: boarded up, three rough planks nailed across (one slanting) over a dark opening, rusty nails. |
| 6 | window_barred | 1536, 0, 256, 512 | 0.50 x 1.00 | Window: small square leaded panes behind three vertical iron bars (a cellar's or a shop's). |
| 7 | window_free | 1792, 0, 256, 512 | 0.50 x 1.00 | Free: another window glass of your choice (stained glass? a round-headed church light?). |
| 8 | shutter_plain | 0, 512, 256, 512 | 0.50 x 1.00 | Shutter: three vertical planks held by two horizontal battens, weathered grey-brown wood, iron hinge straps on the left. |
| 9 | shutter_heart | 256, 512, 256, 512 | 0.50 x 1.00 | Shutter: as the plain one, with a heart cut out in the middle (the heart transparent: alpha 0). |
| 10 | shutter_louvred | 512, 512, 256, 512 | 0.50 x 1.00 | Shutter: a frame with angled louvre slats, painted a faded green. |
| 11 | shutter_braced | 768, 512, 256, 512 | 0.50 x 1.00 | Shutter: planks with two battens and a diagonal brace between them (a Z), a few knots, painted a faded oxblood red. |
| 12 | door_straps | 1024, 512, 256, 512 | 1.00 x 2.00 | Door: vertical planks, two long iron strap hinges with rounded ends, a ring handle, a keyhole plate; the frame is NOT in the cell, only the leaf. |
| 13 | door_studded | 1280, 512, 256, 512 | 1.00 x 2.00 | Door: planks studded with rows of square iron nail heads, a small barred grille at eye height, a heavy ring handle. |
| 14 | door_free | 1536, 512, 256, 512 | 1.00 x 2.00 | Free: another door leaf (a barn's with a Z brace? a fine panelled one?). |
| 15 | door_free_2 | 1792, 512, 256, 512 | 1.00 x 2.00 | Free: another door leaf. |
| 16 | joist_end_plain | 0, 1024, 256, 256 | 0.25 x 0.25 | End grain of a sawn oak joist: growth rings off center, a few radial checks, greyed with age. Fills the cell. |
| 17 | joist_end_chamfered | 256, 1024, 256, 256 | 0.25 x 0.25 | End grain of a joist whose four edges are chamfered: the rings inside a darker bevelled border. |
| 18 | beam_end_rosette | 512, 1024, 256, 256 | 0.30 x 0.30 | The end of a big beam carved with a rosette (a six petal flower in a circle), weathered. |
| 19 | corbel | 768, 1024, 256, 256 | 0.30 x 0.30 | The front of a carved wooden corbel: a scrolled, stepped profile seen head on, shadows in its hollows. |
| 20 | post_head | 1024, 1024, 256, 256 | 0.30 x 0.30 | The face of a post's head carved with chevrons and a small shield. |
| 21 | end_grain_cracked | 1280, 1024, 256, 256 | 0.25 x 0.25 | End grain split by deep radial cracks, dark in the cracks, silver grey outside. |
| 22 | carving_free | 1536, 1024, 256, 256 | 0.30 x 0.30 | Free: another carving (a green man? a date stone?). |
| 23 | carving_free_2 | 1792, 1024, 256, 256 | 0.30 x 0.30 | Free: another carving. |
| 24 | lantern | 0, 1280, 256, 256 | 0.35 x 0.35 | A hanging iron lantern seen from the front, six sided, horn panes glowing warm, a ring on top; outside the lantern transparent (alpha 0). |
| 25 | flower_box | 256, 1280, 256, 256 | 0.60 x 0.60 | The front of a wooden flower box with red and white geraniums spilling over; above the plants transparent (alpha 0). |
| 26 | plaque | 512, 1280, 256, 256 | 0.35 x 0.35 | A small stone plaque with a carved house number (any two digits) and a border, mortared on. |
| 27 | sundial | 768, 1280, 256, 256 | 0.50 x 0.50 | A stone sundial disc with hour lines and roman numerals and an iron gnomon casting a shadow; outside the disc transparent (alpha 0). |
| 28 | horseshoe | 1024, 1280, 256, 256 | 0.25 x 0.25 | An iron horseshoe nailed up, open end up, rusty; outside it transparent (alpha 0). |
| 29 | hook_rope | 1280, 1280, 256, 256 | 0.35 x 0.35 | An iron hook with a coil of hemp rope hung on it; outside transparent (alpha 0). |
| 30 | ornament_free | 1536, 1280, 256, 256 | 0.35 x 0.35 | Free: another ornament (a bell? a wreath? a bird house?). |
| 31 | ornament_free_2 | 1792, 1280, 256, 256 | 0.35 x 0.35 | Free: another ornament. |
| 32 | sign_inn | 0, 1536, 512, 256 | 1.00 x 0.50 | Inn sign: a weathered wooden board with a carved frame and a painted foaming tankard; outside the board transparent (alpha 0). Two iron rings on top. |
| 33 | sign_smith | 512, 1536, 512, 256 | 1.00 x 0.50 | Smith's sign: a board with a painted anvil and hammer. |
| 34 | sign_baker | 1024, 1536, 512, 256 | 1.00 x 0.50 | Baker's sign: a board with a painted pretzel and a loaf. |
| 35 | sign_tailor | 1536, 1536, 512, 256 | 1.00 x 0.50 | Tailor's sign: a board with painted shears and a spool. |
| 36 | sign_herbalist | 0, 1792, 512, 256 | 1.00 x 0.50 | Herbalist's sign: a board with a painted mortar and pestle and a sprig. |
| 37 | sign_boar | 512, 1792, 512, 256 | 1.00 x 0.50 | Tavern sign: a board with a painted boar's head, black on cream. |
| 38 | sign_blank | 1024, 1792, 512, 256 | 1.00 x 0.50 | A blank sign board: the weathered wood and the carved frame only, nothing painted (for lettering added later). |
| 39 | sign_free | 1536, 1792, 512, 256 | 1.00 x 0.50 | Free: another sign of your choice. |

## Prompt to start from

"Paint a texture atlas for a video game, 2048 by 2048 pixels, following the attached template exactly: it is divided
into cells framed in red, and each cell is one separate object seen straight from the front, filling its cell
edge to edge, in flat shadowless light, realistic and weathered, medieval, muted colors, no outlines, no text.
Remove the red frames and labels. Where an object has a shape (a lantern, a horseshoe, a sign board) the
background is transparent. Cell by cell: ..." followed by the table above.
