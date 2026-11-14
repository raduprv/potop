// The houses' details atlas (house_grow.h, house_atlas_*): one 2048 by
// 2048 picture holding the small painted things a house wears: window
// glass, shutters, doors, the ends of beams, ornaments and shop signs,
// each in a cell of its own. The generator maps a window light, a joist's
// end or a hanging sign onto its cell. The atlas is meant to be painted (a
// template with the cells marked and a guide for the painter come out of
// potop_house --atlas); until then a stand-in is painted here from
// nothing, good enough to see the houses by. Each cell comes as color
// (with alpha where the thing has a shape: a lantern, a sign board), a
// normal map and an ARM map derived from a height field, and emissive for
// what glows after dark (the panes of the windows, the lantern).

#include "house_grow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "stb_image_write.h"

#define ATLAS_SIZE 2048
#define ATLAS_INSET 10.0f                 // texels kept from a cell's edge, so the mips (sampled from afar) do not bleed the neighbor in

// A cell: where it is (texels), how big the thing is in the world (meters,
// what 1 by 1 of the cell covers; the painter keeps the scale by it), what
// is in it
typedef struct
{
	const char *name;
	int x;
	int y;
	int width;
	int height;
	float meters_wide;
	float meters_high;
	const char *brief;                     // for the painter's guide
} atlas_cell_t;

static const atlas_cell_t cells[HOUSE_CELL_COUNT] =
{
	// Windows: tall cells, one light of a window each (the lead, the glass; the glass glows at night)
	{ "window_leaded_diamond", 0, 0, 256, 512, 0.5f, 1.0f,
		"Window glass: diamond leaded lights (quarries about 10 cm across), old greenish glass, slightly uneven, lead cames dark grey. Fills the whole cell." },
	{ "window_leaded_square", 256, 0, 256, 512, 0.5f, 1.0f,
		"Window glass: rectangular leaded panes (about 12 by 15 cm), same glass and lead as the first." },
	{ "window_four_pane", 512, 0, 256, 512, 0.5f, 1.0f,
		"Window glass: a wooden cross of muntins (painted wood, 3 cm wide) with four clear panes, a little wavy." },
	{ "window_roundels", 768, 0, 256, 512, 0.5f, 1.0f,
		"Window glass: crown glass roundels (bullseye discs about 11 cm) set in lead, the gaps between filled with small lead-set pieces." },
	{ "window_dirty", 1024, 0, 256, 512, 0.5f, 1.0f,
		"Window glass: diamond leaded as the first, but grimy, a crack across one corner, cobwebs at the top, a quarry replaced by a newer clearer one." },
	{ "window_boarded", 1280, 0, 256, 512, 0.5f, 1.0f,
		"Window: boarded up, three rough planks nailed across (one slanting) over a dark opening, rusty nails." },
	{ "window_barred", 1536, 0, 256, 512, 0.5f, 1.0f,
		"Window: small square leaded panes behind three vertical iron bars (a cellar's or a shop's)." },
	{ "window_free", 1792, 0, 256, 512, 0.5f, 1.0f,
		"Free: another window glass of your choice (stained glass? a round-headed church light?)." },
	// Shutters and doors: tall cells
	{ "shutter_plain", 0, 512, 256, 512, 0.5f, 1.0f,
		"Shutter: three vertical planks held by two horizontal battens, weathered grey-brown wood, iron hinge straps on the left." },
	{ "shutter_heart", 256, 512, 256, 512, 0.5f, 1.0f,
		"Shutter: as the plain one, with a heart cut out in the middle (the heart transparent: alpha 0)." },
	{ "shutter_louvred", 512, 512, 256, 512, 0.5f, 1.0f,
		"Shutter: a frame with angled louvre slats, painted a faded green." },
	{ "shutter_braced", 768, 512, 256, 512, 0.5f, 1.0f,
		"Shutter: planks with two battens and a diagonal brace between them (a Z), a few knots, painted a faded oxblood red." },
	{ "door_straps", 1024, 512, 256, 512, 1.0f, 2.0f,
		"Door: vertical planks, two long iron strap hinges with rounded ends, a ring handle, a keyhole plate; the frame is NOT in the cell, only the leaf." },
	{ "door_studded", 1280, 512, 256, 512, 1.0f, 2.0f,
		"Door: planks studded with rows of square iron nail heads, a small barred grille at eye height, a heavy ring handle." },
	{ "door_free", 1536, 512, 256, 512, 1.0f, 2.0f,
		"Free: another door leaf (a barn's with a Z brace? a fine panelled one?)." },
	{ "door_free_2", 1792, 512, 256, 512, 1.0f, 2.0f,
		"Free: another door leaf." },
	// The ends of beams, and carvings
	{ "joist_end_plain", 0, 1024, 256, 256, 0.25f, 0.25f,
		"End grain of a sawn oak joist: growth rings off center, a few radial checks, greyed with age. Fills the cell." },
	{ "joist_end_chamfered", 256, 1024, 256, 256, 0.25f, 0.25f,
		"End grain of a joist whose four edges are chamfered: the rings inside a darker bevelled border." },
	{ "beam_end_rosette", 512, 1024, 256, 256, 0.3f, 0.3f,
		"The end of a big beam carved with a rosette (a six petal flower in a circle), weathered." },
	{ "corbel", 768, 1024, 256, 256, 0.3f, 0.3f,
		"The front of a carved wooden corbel: a scrolled, stepped profile seen head on, shadows in its hollows." },
	{ "post_head", 1024, 1024, 256, 256, 0.3f, 0.3f,
		"The face of a post's head carved with chevrons and a small shield." },
	{ "end_grain_cracked", 1280, 1024, 256, 256, 0.25f, 0.25f,
		"End grain split by deep radial cracks, dark in the cracks, silver grey outside." },
	{ "carving_free", 1536, 1024, 256, 256, 0.3f, 0.3f,
		"Free: another carving (a green man? a date stone?)." },
	{ "carving_free_2", 1792, 1024, 256, 256, 0.3f, 0.3f,
		"Free: another carving." },
	// Ornaments: things hung on a wall, with a shape (alpha)
	{ "lantern", 0, 1280, 256, 256, 0.35f, 0.35f,
		"A hanging iron lantern seen from the front, six sided, horn panes glowing warm, a ring on top; outside the lantern transparent (alpha 0)." },
	{ "flower_box", 256, 1280, 256, 256, 0.6f, 0.6f,
		"The front of a wooden flower box with red and white geraniums spilling over; above the plants transparent (alpha 0)." },
	{ "plaque", 512, 1280, 256, 256, 0.35f, 0.35f,
		"A small stone plaque with a carved house number (any two digits) and a border, mortared on." },
	{ "sundial", 768, 1280, 256, 256, 0.5f, 0.5f,
		"A stone sundial disc with hour lines and roman numerals and an iron gnomon casting a shadow; outside the disc transparent (alpha 0)." },
	{ "horseshoe", 1024, 1280, 256, 256, 0.25f, 0.25f,
		"An iron horseshoe nailed up, open end up, rusty; outside it transparent (alpha 0)." },
	{ "hook_rope", 1280, 1280, 256, 256, 0.35f, 0.35f,
		"An iron hook with a coil of hemp rope hung on it; outside transparent (alpha 0)." },
	{ "ornament_free", 1536, 1280, 256, 256, 0.35f, 0.35f,
		"Free: another ornament (a bell? a wreath? a bird house?)." },
	{ "ornament_free_2", 1792, 1280, 256, 256, 0.35f, 0.35f,
		"Free: another ornament." },
	// Signs: wide cells, a hanging sign board each, its shape in the alpha
	{ "sign_inn", 0, 1536, 512, 256, 1.0f, 0.5f,
		"Inn sign: a weathered wooden board with a carved frame and a painted foaming tankard; outside the board transparent (alpha 0). Two iron rings on top." },
	{ "sign_smith", 512, 1536, 512, 256, 1.0f, 0.5f,
		"Smith's sign: a board with a painted anvil and hammer." },
	{ "sign_baker", 1024, 1536, 512, 256, 1.0f, 0.5f,
		"Baker's sign: a board with a painted pretzel and a loaf." },
	{ "sign_tailor", 1536, 1536, 512, 256, 1.0f, 0.5f,
		"Tailor's sign: a board with painted shears and a spool." },
	{ "sign_herbalist", 0, 1792, 512, 256, 1.0f, 0.5f,
		"Herbalist's sign: a board with a painted mortar and pestle and a sprig." },
	{ "sign_boar", 512, 1792, 512, 256, 1.0f, 0.5f,
		"Tavern sign: a board with a painted boar's head, black on cream." },
	{ "sign_blank", 1024, 1792, 512, 256, 1.0f, 0.5f,
		"A blank sign board: the weathered wood and the carved frame only, nothing painted (for lettering added later)." },
	{ "sign_free", 1536, 1792, 512, 256, 1.0f, 0.5f,
		"Free: another sign of your choice." },
};

int house_atlas_cell_count(void)
{
	return HOUSE_CELL_COUNT;
}

const char *house_atlas_cell_name(int cell)
{
	return cell >= 0 && cell < HOUSE_CELL_COUNT ? cells[cell].name : NULL;
}

void house_atlas_uv(int cell, float s, float t, float uv[2])
{
	const atlas_cell_t *c = &cells[cell < 0 || cell >= HOUSE_CELL_COUNT ? 0 : cell];
	float inset_s = ATLAS_INSET / (float)c->width;
	float inset_t = ATLAS_INSET / (float)c->height;

	s = inset_s + (1.0f - 2.0f * inset_s) * (s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s));
	t = inset_t + (1.0f - 2.0f * inset_t) * (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
	uv[0] = ((float)c->x + s * (float)c->width) / (float)ATLAS_SIZE;
	uv[1] = ((float)c->y + t * (float)c->height) / (float)ATLAS_SIZE;
}

void house_atlas_cell_meters(int cell, float *wide, float *high)
{
	const atlas_cell_t *c = &cells[cell < 0 || cell >= HOUSE_CELL_COUNT ? 0 : cell];

	*wide = c->meters_wide;
	*high = c->meters_high;
}

// ---------------------------------------------------------------------------
// The stand-in painter

typedef struct
{
	float color[3];
	float alpha;
	float height;
	float roughness;
	float metal;
	float glow[3];
} texel_t;

static float clampf(float value, float low, float high)
{
	return value < low ? low : (value > high ? high : value);
}

static float smoothstep_of(float low, float high, float value)
{
	float t = clampf((value - low) / (high - low), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

static float mixf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static uint32_t hash_of(int x, int y, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;

	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static float hash_unit_of(int x, int y, uint32_t seed)
{
	return (float)(hash_of(x, y, seed) & 0xFFFFu) / 65535.0f;
}

// Smooth noise in 0..1 over a plane (meters, not tiling: a cell is one picture)
static float noise_of(float x, float y, uint32_t seed)
{
	int ix = (int)floorf(x);
	int iy = (int)floorf(y);
	float fx = x - (float)ix;
	float fy = y - (float)iy;
	float ux = fx * fx * (3.0f - 2.0f * fx);
	float uy = fy * fy * (3.0f - 2.0f * fy);
	float a = hash_unit_of(ix, iy, seed);
	float b = hash_unit_of(ix + 1, iy, seed);
	float c = hash_unit_of(ix, iy + 1, seed);
	float d = hash_unit_of(ix + 1, iy + 1, seed);

	return mixf(mixf(a, b, ux), mixf(c, d, ux), uy);
}

static float fractal_of(float x, float y, int octaves, uint32_t seed)
{
	float sum = 0.0f;
	float amplitude = 0.5f;
	float total = 0.0f;

	for (int i = 0; i < octaves; i++)
	{
		sum += noise_of(x, y, seed + (uint32_t)i * 17u) * amplitude;
		total += amplitude;
		x *= 2.03f;
		y *= 1.97f;
		amplitude *= 0.5f;
	}
	return sum / total;
}

static void set3(float out[3], float r, float g, float b)
{
	out[0] = r;
	out[1] = g;
	out[2] = b;
}

static void mix3(float out[3], const float a[3], const float b[3], float t)
{
	for (int k = 0; k < 3; k++)
	{
		out[k] = a[k] + (b[k] - a[k]) * t;
	}
}

static void scale3(float out[3], float by)
{
	for (int k = 0; k < 3; k++)
	{
		out[k] *= by;
	}
}

// Wood, by the meter: grain along y (the planks stand up), a plank's
// color drifting from one to the next (plank: which one)
static void wood_texel(float x, float y, int plank, float grey, texel_t *texel)
{
	float ring = fractal_of(x * 40.0f + (float)plank * 7.3f, y * 2.5f, 3, 11u + (uint32_t)plank);
	float grain = 0.5f + 0.5f * sinf((x * 60.0f + ring * 6.0f + (float)plank * 2.1f) * 3.0f);
	float knots = fractal_of(x * 8.0f, y * 8.0f, 2, 31u + (uint32_t)plank);
	float tone = hash_unit_of(plank, 3, 5u);
	float light[3];
	float dark[3];
	float silver[3];

	set3(light, 0.52f, 0.38f, 0.22f);
	set3(dark, 0.3f, 0.2f, 0.11f);
	set3(silver, 0.5f, 0.48f, 0.42f);
	mix3(texel->color, light, dark, grain * 0.6f + ring * 0.4f);
	scale3(texel->color, 0.85f + tone * 0.3f);
	mix3(texel->color, texel->color, silver, grey * (0.5f + 0.5f * knots));
	texel->height = 0.5f + (grain - 0.5f) * 0.1f + (ring - 0.5f) * 0.15f;
	texel->roughness = 0.75f + grain * 0.15f;
	texel->metal = 0.0f;
	texel->alpha = 1.0f;
	set3(texel->glow, 0.0f, 0.0f, 0.0f);
}

static void iron_texel(float x, float y, texel_t *texel)
{
	float rust = fractal_of(x * 30.0f, y * 30.0f, 3, 77u);
	float dark[3];
	float rusty[3];

	set3(dark, 0.08f, 0.08f, 0.085f);
	set3(rusty, 0.32f, 0.16f, 0.07f);
	mix3(texel->color, dark, rusty, smoothstep_of(0.5f, 0.8f, rust));
	texel->height = 0.7f;
	texel->roughness = 0.55f + rust * 0.35f;
	texel->metal = 0.6f * (1.0f - smoothstep_of(0.5f, 0.8f, rust));
	texel->alpha = 1.0f;
	set3(texel->glow, 0.0f, 0.0f, 0.0f);
}

static void stone_texel(float x, float y, texel_t *texel)
{
	float grain = fractal_of(x * 25.0f, y * 25.0f, 4, 91u);
	float light[3];
	float dark[3];

	set3(light, 0.62f, 0.6f, 0.55f);
	set3(dark, 0.42f, 0.41f, 0.38f);
	mix3(texel->color, light, dark, grain);
	texel->height = 0.5f + (grain - 0.5f) * 0.1f;
	texel->roughness = 0.9f;
	texel->metal = 0.0f;
	texel->alpha = 1.0f;
	set3(texel->glow, 0.0f, 0.0f, 0.0f);
}

// Old glass between lead: a pane's own tint, a wobble, the lead cames
// raised and dark; lead: 1 on a came, 0 on the glass
static void glass_texel(float x, float y, int pane, float lead, float dirt, texel_t *texel)
{
	float wobble = fractal_of(x * 20.0f + (float)pane * 3.1f, y * 20.0f, 3, 7u);
	float tint = hash_unit_of(pane, 11, 3u);
	float clear[3];
	float green[3];
	float grime[3];
	float came[3];
	float warm[3];

	set3(clear, 0.5f, 0.56f, 0.6f);
	set3(green, 0.42f, 0.5f, 0.42f);
	set3(grime, 0.3f, 0.28f, 0.22f);
	set3(came, 0.16f, 0.16f, 0.17f);
	set3(warm, 1.0f, 0.72f, 0.42f);
	mix3(texel->color, clear, green, tint);
	scale3(texel->color, 0.8f + wobble * 0.4f);
	mix3(texel->color, texel->color, grime, dirt);
	texel->height = 0.5f + (wobble - 0.5f) * 0.06f;
	texel->roughness = 0.12f + dirt * 0.5f;
	texel->metal = 0.0f;
	// The glow: the room's light through the pane, each pane a little different
	mix3(texel->glow, warm, warm, 0.0f);
	scale3(texel->glow, 0.35f * (0.55f + 0.45f * tint) * (0.6f + 0.4f * wobble) * (1.0f - dirt * 0.7f));
	// The lead
	mix3(texel->color, texel->color, came, lead);
	texel->height = mixf(texel->height, 0.75f, lead);
	texel->roughness = mixf(texel->roughness, 0.6f, lead);
	scale3(texel->glow, 1.0f - lead);
	texel->alpha = 1.0f;
}

// Diamond lattice: which quarry (s, t in meters), and how close to a came
static float diamond_lattice(float x, float y, float pitch, float came, int *pane_out)
{
	float a = (x + y) / pitch;
	float b = (x - y) / pitch;
	int ia = (int)floorf(a);
	int ib = (int)floorf(b);
	float da = fabsf(a - floorf(a + 0.5f)) * pitch * 0.7071f;
	float db = fabsf(b - floorf(b + 0.5f)) * pitch * 0.7071f;
	float d = da < db ? da : db;

	*pane_out = ia * 131 + ib;
	return 1.0f - smoothstep_of(came * 0.5f, came * 0.5f + 0.002f, d);
}

static float square_lattice(float x, float y, float pitch_x, float pitch_y, float came, int *pane_out)
{
	float dx = fabsf(x / pitch_x - floorf(x / pitch_x + 0.5f)) * pitch_x;
	float dy = fabsf(y / pitch_y - floorf(y / pitch_y + 0.5f)) * pitch_y;
	float d = dx < dy ? dx : dy;

	*pane_out = (int)floorf(x / pitch_x) * 131 + (int)floorf(y / pitch_y);
	return 1.0f - smoothstep_of(came * 0.5f, came * 0.5f + 0.002f, d);
}

// A rounded rectangle's inside (1) with a soft edge, in the cell's 0..1
static float inside_box(float s, float t, float s0, float t0, float s1, float t1, float soft)
{
	float a = smoothstep_of(s0 - soft, s0 + soft, s) * (1.0f - smoothstep_of(s1 - soft, s1 + soft, s));
	float b = smoothstep_of(t0 - soft, t0 + soft, t) * (1.0f - smoothstep_of(t1 - soft, t1 + soft, t));

	return a * b;
}

static float inside_disc(float s, float t, float cs, float ct, float radius, float soft)
{
	float d = sqrtf((s - cs) * (s - cs) + (t - ct) * (t - ct));

	return 1.0f - smoothstep_of(radius - soft, radius + soft, d);
}

// Planks standing up across the cell (count of them), with battens across
// (batten_t: their centers in t; batten_count), a rough edge between planks
static void planks_texel(float s, float t, float wide, float high, int count, const float *batten_t, int batten_count, float batten_height,
	float grey, texel_t *texel)
{
	float x = s * wide;
	float y = t * high;
	int plank = (int)floorf(s * (float)count);
	float in_plank = s * (float)count - (float)plank;
	float gap = 1.0f - inside_box(in_plank, 0.5f, 0.02f, 0.0f, 0.98f, 1.0f, 0.01f);

	wood_texel(x, y, plank, grey, texel);
	texel->height -= gap * 0.3f;
	scale3(texel->color, 1.0f - gap * 0.5f);
	for (int i = 0; i < batten_count; i++)
	{
		float on = inside_box(s, t, 0.04f, batten_t[i] - batten_height * 0.5f, 0.96f, batten_t[i] + batten_height * 0.5f, 0.004f);

		if (on > 0.0f)
		{
			texel_t batten;

			wood_texel(y + 1.3f, x, 20 + i, grey, &batten);
			mix3(texel->color, texel->color, batten.color, on);
			texel->height = mixf(texel->height, batten.height + 0.25f, on);
			// Nails at each plank
			for (int p = 0; p < count; p++)
			{
				float nail = inside_disc(s, t, ((float)p + 0.5f) / (float)count, batten_t[i], 0.012f, 0.004f) * on;

				if (nail > 0.0f)
				{
					texel_t head;

					iron_texel(x, y, &head);
					mix3(texel->color, texel->color, head.color, nail);
					texel->height = mixf(texel->height, 0.95f, nail);
					texel->metal = mixf(texel->metal, head.metal, nail);
					texel->roughness = mixf(texel->roughness, head.roughness, nail);
				}
			}
		}
	}
}

// A stroke along a segment, in the cell's 0..1 (aspect corrected by the
// cell's shape through sx: multiply s by it first)
static float along_segment(float s, float t, float s0, float t0, float s1, float t1, float width)
{
	float dx = s1 - s0;
	float dy = t1 - t0;
	float length2 = dx * dx + dy * dy;
	float u = length2 > 0.0f ? clampf(((s - s0) * dx + (t - t0) * dy) / length2, 0.0f, 1.0f) : 0.0f;
	float px = s0 + dx * u;
	float py = t0 + dy * u;
	float d = sqrtf((s - px) * (s - px) + (t - py) * (t - py));

	return 1.0f - smoothstep_of(width - 0.004f, width + 0.004f, d);
}

static void paint_over(texel_t *texel, const texel_t *over, float amount)
{
	mix3(texel->color, texel->color, over->color, amount);
	texel->height = mixf(texel->height, over->height, amount);
	texel->roughness = mixf(texel->roughness, over->roughness, amount);
	texel->metal = mixf(texel->metal, over->metal, amount);
	mix3(texel->glow, texel->glow, over->glow, amount);
}

// Paint, flat colored, with a little grain, laid on wood (a sign's picture)
static void paint_texel(const float color[3], float x, float y, texel_t *texel)
{
	float wear = fractal_of(x * 50.0f, y * 50.0f, 3, 123u);

	set3(texel->color, color[0], color[1], color[2]);
	scale3(texel->color, 0.85f + wear * 0.3f);
	texel->height = 0.55f;
	texel->roughness = 0.6f + wear * 0.2f;
	texel->metal = 0.0f;
	texel->alpha = 1.0f;
	set3(texel->glow, 0.0f, 0.0f, 0.0f);
}

// A sign board: horizontal planks in a frame, the shape in the alpha
static void sign_board(float s, float t, float wide, float high, texel_t *texel)
{
	float shape = inside_box(s, t, 0.03f, 0.08f, 0.97f, 0.97f, 0.006f);
	float frame = shape - inside_box(s, t, 0.08f, 0.17f, 0.92f, 0.9f, 0.006f);
	int plank = (int)floorf(t * 3.0f);

	wood_texel(t * high + 0.4f, s * wide, plank, 0.5f, texel);
	if (frame > 0.0f)
	{
		texel_t edge;

		wood_texel(s * wide, t * high, 9, 0.3f, &edge);
		edge.height = 0.8f;
		scale3(edge.color, 0.8f);
		paint_over(texel, &edge, frame);
	}
	// The two rings on top
	for (int r = 0; r < 2; r++)
	{
		float cs = r == 0 ? 0.25f : 0.75f;
		float ring = inside_disc(s * 2.0f, t, cs * 2.0f, 0.06f, 0.07f, 0.008f) - inside_disc(s * 2.0f, t, cs * 2.0f, 0.06f, 0.045f, 0.008f);

		if (ring > 0.0f)
		{
			texel_t iron;

			iron_texel(s * wide, t * high, &iron);
			paint_over(texel, &iron, ring);
			shape = shape > ring ? shape : ring;
		}
	}
	texel->alpha = shape;
}

static void paint_sign_symbol(int cell, float s, float t, float wide, float high, texel_t *texel)
{
	float cream[3];
	float black[3];
	float red[3];
	float brown[3];
	float amount = 0.0f;
	const float *color = cream;
	// The picture is drawn in a square frame in the middle of the board,
	// u and v in 0..1
	float u = (s - 0.3f) / 0.4f;
	float v = (t - 0.2f) / 0.7f;
	float su = u * 0.5f;                   // the board is twice as wide as high: distances in v's scale

	set3(cream, 0.85f, 0.8f, 0.65f);
	set3(black, 0.08f, 0.07f, 0.06f);
	set3(red, 0.6f, 0.12f, 0.08f);
	switch (cell)
	{
	case HOUSE_CELL_SIGN_INN:
		// A tankard: a body, a handle, foam on top
		amount = inside_box(u, v, 0.3f, 0.3f, 0.7f, 0.95f, 0.01f);
		amount += (inside_disc(su, v, 0.37f, 0.6f, 0.18f, 0.01f) - inside_disc(su, v, 0.37f, 0.6f, 0.11f, 0.01f)) * (su > 0.36f ? 1.0f : 0.0f);
		amount = clampf(amount, 0.0f, 1.0f);
		{
			float foam = inside_disc(su, v, 0.19f, 0.28f, 0.1f, 0.01f) + inside_disc(su, v, 0.28f, 0.22f, 0.1f, 0.01f) + inside_disc(su, v, 0.11f, 0.24f, 0.08f, 0.01f);

			if (foam > amount)
			{
				amount = clampf(foam, 0.0f, 1.0f);
				color = cream;
			}
		}
		break;
	case HOUSE_CELL_SIGN_SMITH:
		// An anvil: the body, the horn, the foot
		amount = inside_box(u, v, 0.15f, 0.3f, 0.75f, 0.5f, 0.01f) + inside_box(u, v, 0.3f, 0.5f, 0.6f, 0.7f, 0.01f) + inside_box(u, v, 0.2f, 0.7f, 0.7f, 0.85f, 0.01f)
			+ inside_disc(su, v, 0.42f, 0.4f, 0.1f, 0.01f);
		amount = clampf(amount, 0.0f, 1.0f);
		color = black;
		break;
	case HOUSE_CELL_SIGN_BAKER:
		// A pretzel: three rings
		amount = inside_disc(su, v, 0.17f, 0.65f, 0.16f, 0.01f) - inside_disc(su, v, 0.17f, 0.65f, 0.09f, 0.01f);
		amount += inside_disc(su, v, 0.33f, 0.65f, 0.16f, 0.01f) - inside_disc(su, v, 0.33f, 0.65f, 0.09f, 0.01f);
		amount += inside_disc(su, v, 0.25f, 0.38f, 0.17f, 0.01f) - inside_disc(su, v, 0.25f, 0.38f, 0.1f, 0.01f);
		amount = clampf(amount, 0.0f, 1.0f);
		set3(brown, 0.55f, 0.32f, 0.12f);
		color = brown;
		break;
	case HOUSE_CELL_SIGN_TAILOR:
		// Shears: two blades crossing, two rings
		amount = along_segment(su, v, 0.1f, 0.2f, 0.4f, 0.85f, 0.035f) + along_segment(su, v, 0.4f, 0.2f, 0.1f, 0.85f, 0.035f);
		amount += inside_disc(su, v, 0.08f, 0.88f, 0.08f, 0.01f) - inside_disc(su, v, 0.08f, 0.88f, 0.045f, 0.01f);
		amount += inside_disc(su, v, 0.42f, 0.88f, 0.08f, 0.01f) - inside_disc(su, v, 0.42f, 0.88f, 0.045f, 0.01f);
		amount = clampf(amount, 0.0f, 1.0f);
		color = black;
		break;
	case HOUSE_CELL_SIGN_HERBALIST:
		// A mortar and its pestle, a sprig
		amount = inside_disc(su, v, 0.25f, 0.55f, 0.2f, 0.01f) * (v > 0.55f ? 1.0f : 0.0f) + inside_box(u, v, 0.2f, 0.5f, 0.8f, 0.58f, 0.01f);
		amount += along_segment(su, v, 0.25f, 0.6f, 0.42f, 0.15f, 0.03f);
		amount += along_segment(su, v, 0.08f, 0.5f, 0.14f, 0.2f, 0.012f);
		amount = clampf(amount, 0.0f, 1.0f);
		color = cream;
		break;
	case HOUSE_CELL_SIGN_BOAR:
		// A boar's head, roughly: the head, the snout, an ear, a tusk
		amount = inside_disc(su, v, 0.22f, 0.5f, 0.2f, 0.01f) + inside_box(u, v, 0.55f, 0.5f, 0.85f, 0.7f, 0.02f) + along_segment(su, v, 0.12f, 0.33f, 0.06f, 0.15f, 0.04f);
		amount = clampf(amount, 0.0f, 1.0f);
		color = black;
		{
			float tusk = along_segment(su, v, 0.4f, 0.72f, 0.44f, 0.55f, 0.015f);

			if (tusk > amount)
			{
				amount = tusk;
				color = cream;
			}
		}
		break;
	default:
		return;
	}
	if (amount > 0.0f)
	{
		texel_t paint;

		paint_texel(color, s * wide, t * high, &paint);
		paint_over(texel, &paint, amount);
	}
}

// The texel of a cell at (s, t) in 0..1 across it
static void cell_texel(int cell, float s, float t, texel_t *texel)
{
	const atlas_cell_t *c = &cells[cell];
	float wide = c->meters_wide;
	float high = c->meters_high;
	float x = s * wide;
	float y = t * high;
	int pane;
	float lead;

	switch ((house_cell_t)cell)
	{
	case HOUSE_CELL_WINDOW_LEADED_DIAMOND:
		lead = diamond_lattice(x, y, 0.1f, 0.006f, &pane);
		glass_texel(x, y, pane, lead, 0.0f, texel);
		break;
	case HOUSE_CELL_WINDOW_LEADED_SQUARE:
		lead = square_lattice(x, y, 0.125f, 0.1667f, 0.006f, &pane);
		glass_texel(x, y, pane, lead, 0.0f, texel);
		break;
	case HOUSE_CELL_WINDOW_FOUR_PANE:
	{
		float bar = square_lattice(x, y, wide, high, 0.03f, &pane) + inside_box(s, t, 0.47f, 0.0f, 0.53f, 1.0f, 0.003f) + inside_box(s, t, 0.0f, 0.47f, 1.0f, 0.53f, 0.003f);

		glass_texel(x, y, pane, 0.0f, 0.0f, texel);
		if (bar > 0.0f)
		{
			texel_t wood;
			float paint[3];

			wood_texel(x, y, 3, 0.2f, &wood);
			set3(paint, 0.7f, 0.68f, 0.6f);
			mix3(wood.color, wood.color, paint, 0.7f);
			wood.height = 0.8f;
			wood.roughness = 0.5f;
			paint_over(texel, &wood, clampf(bar, 0.0f, 1.0f));
		}
		break;
	}
	case HOUSE_CELL_WINDOW_ROUNDELS:
	{
		// Discs in a staggered grid, lead round them and between
		float pitch = 0.115f;
		int row = (int)floorf(y / pitch);
		float cx = (floorf(x / pitch - (row % 2 == 0 ? 0.0f : 0.5f)) + (row % 2 == 0 ? 0.5f : 1.0f)) * pitch;
		float cy = ((float)row + 0.5f) * pitch;
		float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
		float in_disc = 1.0f - smoothstep_of(0.05f, 0.054f, d);
		float ring = sinf(d * 400.0f) * 0.5f + 0.5f;

		pane = row * 131 + (int)floorf(cx / pitch);
		glass_texel(x, y, pane, 1.0f - in_disc, 0.0f, texel);
		texel->height += in_disc * (0.06f * (1.0f - d / 0.054f) + ring * 0.02f);
		scale3(texel->color, 1.0f - in_disc * ring * 0.15f);
		break;
	}
	case HOUSE_CELL_WINDOW_DIRTY:
	{
		float dirt = smoothstep_of(0.2f, 1.0f, fractal_of(x * 9.0f, y * 9.0f, 3, 44u)) * 0.8f + (1.0f - t) * 0.2f;
		float crack = along_segment(s * 0.5f, t, 0.02f, 0.05f, 0.3f, 0.4f, 0.003f) + along_segment(s * 0.5f, t, 0.3f, 0.4f, 0.2f, 0.6f, 0.0025f);

		lead = diamond_lattice(x, y, 0.1f, 0.006f, &pane);
		glass_texel(x, y, pane, lead, clampf(dirt, 0.0f, 1.0f), texel);
		scale3(texel->color, 1.0f - clampf(crack, 0.0f, 1.0f) * 0.5f);
		texel->height -= clampf(crack, 0.0f, 1.0f) * 0.1f;
		break;
	}
	case HOUSE_CELL_WINDOW_BOARDED:
	{
		float dark[3];
		float boards = 0.0f;

		lead = diamond_lattice(x, y, 0.1f, 0.006f, &pane);
		glass_texel(x, y, pane, lead, 0.6f, texel);
		set3(dark, 0.05f, 0.05f, 0.05f);
		mix3(texel->color, texel->color, dark, 0.7f);
		scale3(texel->glow, 0.1f);
		for (int i = 0; i < 3; i++)
		{
			float t0 = 0.18f + 0.3f * (float)i;
			float slant = i == 1 ? 0.12f : 0.0f;
			float on = along_segment(s * 0.5f, t, -0.02f, t0 - slant, 0.52f, t0 + slant, 0.06f);

			if (on > 0.0f)
			{
				texel_t plank;

				wood_texel(y + (float)i * 0.7f, x, 30 + i, 0.6f, &plank);
				plank.height = 0.9f;
				paint_over(texel, &plank, on);
				scale3(texel->glow, 1.0f - on);
				boards = boards > on ? boards : on;
			}
		}
		break;
	}
	case HOUSE_CELL_WINDOW_BARRED:
		lead = square_lattice(x, y, 0.1f, 0.1f, 0.006f, &pane);
		glass_texel(x, y, pane, lead, 0.2f, texel);
		for (int i = 0; i < 3; i++)
		{
			float on = inside_box(s, t, 0.25f * (float)(i + 1) - 0.035f, 0.0f, 0.25f * (float)(i + 1) + 0.035f, 1.0f, 0.004f);

			if (on > 0.0f)
			{
				texel_t bar;

				iron_texel(x, y, &bar);
				bar.height = 0.95f;
				paint_over(texel, &bar, on);
				scale3(texel->glow, 1.0f - on);
			}
		}
		break;
	case HOUSE_CELL_SHUTTER_PLAIN:
	case HOUSE_CELL_SHUTTER_HEART:
	case HOUSE_CELL_SHUTTER_BRACED:
	{
		static const float battens[2] = { 0.2f, 0.8f };

		planks_texel(s, t, wide, high, 3, battens, 2, 0.07f, cell == HOUSE_CELL_SHUTTER_BRACED ? 0.2f : 0.6f, texel);
		if (cell == HOUSE_CELL_SHUTTER_BRACED)
		{
			float on = along_segment(s * 0.5f, t, 0.06f, 0.76f, 0.44f, 0.24f, 0.035f);
			float red[3];

			if (on > 0.0f)
			{
				texel_t brace;

				wood_texel(x + 0.9f, y, 40, 0.2f, &brace);
				brace.height = 0.8f;
				paint_over(texel, &brace, on);
			}
			set3(red, 0.45f, 0.14f, 0.1f);
			mix3(texel->color, texel->color, red, 0.55f * (0.7f + 0.3f * fractal_of(x * 30.0f, y * 30.0f, 2, 5u)));
		}
		if (cell == HOUSE_CELL_SHUTTER_HEART)
		{
			// Two discs and a point, in the cell's middle
			float hs = (s - 0.5f) * 0.5f;
			float ht = t - 0.5f;
			float heart = inside_disc(hs, ht, -0.045f, -0.03f, 0.05f, 0.004f) + inside_disc(hs, ht, 0.045f, -0.03f, 0.05f, 0.004f);
			float point = (ht > -0.03f && ht < 0.11f && fabsf(hs) < (0.11f - ht) * 0.68f) ? 1.0f : 0.0f;

			texel->alpha = 1.0f - clampf(heart + point, 0.0f, 1.0f);
		}
		// Hinge straps on the left
		for (int i = 0; i < 2; i++)
		{
			float on = inside_box(s, t, 0.0f, battens[i] - 0.02f, 0.45f, battens[i] + 0.02f, 0.004f);

			if (on > 0.0f)
			{
				texel_t strap;

				iron_texel(x, y, &strap);
				strap.height = 0.98f;
				paint_over(texel, &strap, on);
			}
		}
		break;
	}
	case HOUSE_CELL_SHUTTER_LOUVRED:
	{
		float frame = 1.0f - inside_box(s, t, 0.12f, 0.06f, 0.88f, 0.94f, 0.004f);
		float slat = fmodf(t * 18.0f, 1.0f);
		float shade = 0.55f + 0.45f * slat;
		float green[3];

		wood_texel(x, y, 2, 0.3f, texel);
		set3(green, 0.3f, 0.42f, 0.3f);
		mix3(texel->color, texel->color, green, 0.6f);
		if (frame < 1.0f)
		{
			scale3(texel->color, shade);
			texel->height = 0.3f + slat * 0.35f;
		}
		else
		{
			texel->height = 0.75f;
		}
		break;
	}
	case HOUSE_CELL_DOOR_STRAPS:
	case HOUSE_CELL_DOOR_STUDDED:
	{
		planks_texel(s, t, wide, high, 5, NULL, 0, 0.0f, 0.35f, texel);
		if (cell == HOUSE_CELL_DOOR_STRAPS)
		{
			for (int i = 0; i < 2; i++)
			{
				float ct = 0.22f + 0.56f * (float)i;
				float on = inside_box(s, t, 0.0f, ct - 0.022f, 0.75f, ct + 0.022f, 0.003f) + inside_disc(s * 0.5f, t, 0.375f, ct, 0.035f, 0.004f);

				if (on > 0.0f)
				{
					texel_t strap;

					iron_texel(x, y, &strap);
					strap.height = 0.97f;
					paint_over(texel, &strap, clampf(on, 0.0f, 1.0f));
				}
			}
			// The ring handle and the keyhole plate
			{
				float ring = inside_disc(s * 0.5f, t, 0.38f, 0.52f, 0.045f, 0.004f) - inside_disc(s * 0.5f, t, 0.38f, 0.52f, 0.03f, 0.004f);
				float plate = inside_box(s, t, 0.7f, 0.55f, 0.84f, 0.63f, 0.003f);
				float on = clampf(ring + plate, 0.0f, 1.0f);

				if (on > 0.0f)
				{
					texel_t iron;

					iron_texel(x, y, &iron);
					iron.height = 0.98f;
					paint_over(texel, &iron, on);
				}
			}
		}
		else
		{
			// Rows of square nail heads
			float gs = fmodf(s * 8.0f, 1.0f);
			float gt = fmodf(t * 16.0f, 1.0f);
			float head = inside_box(gs, gt, 0.3f, 0.3f, 0.7f, 0.7f, 0.1f);
			float grille = inside_box(s, t, 0.3f, 0.2f, 0.7f, 0.32f, 0.003f);

			if (head > 0.0f)
			{
				texel_t iron;

				iron_texel(x, y, &iron);
				iron.height = 0.95f;
				paint_over(texel, &iron, head);
			}
			if (grille > 0.0f)
			{
				texel_t iron;
				float bars = fmodf(s * 20.0f, 1.0f) < 0.3f ? 1.0f : 0.0f;

				iron_texel(x, y, &iron);
				scale3(iron.color, bars);
				iron.height = bars * 0.9f;
				paint_over(texel, &iron, grille);
			}
			{
				float ring = inside_disc(s * 0.5f, t, 0.38f, 0.55f, 0.05f, 0.004f) - inside_disc(s * 0.5f, t, 0.38f, 0.55f, 0.033f, 0.004f);

				if (ring > 0.0f)
				{
					texel_t iron;

					iron_texel(x, y, &iron);
					iron.height = 0.98f;
					paint_over(texel, &iron, ring);
				}
			}
		}
		break;
	}
	case HOUSE_CELL_JOIST_END_PLAIN:
	case HOUSE_CELL_JOIST_END_CHAMFERED:
	case HOUSE_CELL_END_GRAIN_CRACKED:
	{
		// Growth rings round a center off to one side
		float cx = 0.3f;
		float cy = 0.62f;
		float d = sqrtf((s - cx) * (s - cx) + (t - cy) * (t - cy));
		float wobble = fractal_of(s * 6.0f, t * 6.0f, 2, 9u) * 0.03f;
		float ring = 0.5f + 0.5f * sinf((d + wobble) * 110.0f);
		float light[3];
		float dark[3];
		float silver[3];
		float angle = atan2f(t - cy, s - cx);
		float cracks = 0.0f;

		set3(light, 0.55f, 0.42f, 0.26f);
		set3(dark, 0.33f, 0.23f, 0.13f);
		set3(silver, 0.5f, 0.47f, 0.42f);
		mix3(texel->color, light, dark, ring);
		mix3(texel->color, texel->color, silver, 0.45f);
		texel->height = 0.5f + (ring - 0.5f) * 0.08f;
		texel->roughness = 0.85f;
		texel->metal = 0.0f;
		texel->alpha = 1.0f;
		set3(texel->glow, 0.0f, 0.0f, 0.0f);
		// Radial checks: a few, from the center out, wider outward
		for (int i = 0; i < (cell == HOUSE_CELL_END_GRAIN_CRACKED ? 7 : 3); i++)
		{
			float at = -3.14159f + 6.2832f * hash_unit_of(i, 2, 61u);
			float difference = fabsf(angle - at);
			float width = (cell == HOUSE_CELL_END_GRAIN_CRACKED ? 0.06f : 0.025f) * d;

			difference = difference > 3.14159f ? 6.2832f - difference : difference;
			if (d > 0.05f && difference * d < width)
			{
				cracks = cracks > (1.0f - difference * d / width) ? cracks : (1.0f - difference * d / width);
			}
		}
		scale3(texel->color, 1.0f - cracks * 0.8f);
		texel->height -= cracks * 0.4f;
		if (cell == HOUSE_CELL_JOIST_END_CHAMFERED)
		{
			float bevel = 1.0f - inside_box(s, t, 0.12f, 0.12f, 0.88f, 0.88f, 0.01f);

			if (bevel > 0.0f)
			{
				texel_t face;

				wood_texel(x, y, 7, 0.5f, &face);
				face.height = 0.3f + 0.2f * (1.0f - fabsf(s - 0.5f) - fabsf(t - 0.5f));
				scale3(face.color, 0.8f);
				paint_over(texel, &face, bevel);
			}
		}
		break;
	}
	case HOUSE_CELL_BEAM_END_ROSETTE:
	case HOUSE_CELL_POST_HEAD:
	case HOUSE_CELL_CORBEL:
	{
		wood_texel(x, y, 5, 0.5f, texel);
		if (cell == HOUSE_CELL_BEAM_END_ROSETTE)
		{
			float d = sqrtf((s - 0.5f) * (s - 0.5f) + (t - 0.5f) * (t - 0.5f));
			float angle = atan2f(t - 0.5f, s - 0.5f);
			float petals = 0.5f + 0.5f * cosf(angle * 6.0f);
			float flower = (1.0f - smoothstep_of(0.2f + petals * 0.14f, 0.22f + petals * 0.14f, d)) * smoothstep_of(0.05f, 0.07f, d);
			float circle = inside_disc(s, t, 0.5f, 0.5f, 0.43f, 0.01f) - inside_disc(s, t, 0.5f, 0.5f, 0.39f, 0.01f);

			texel->height += flower * 0.3f + circle * 0.25f + inside_disc(s, t, 0.5f, 0.5f, 0.05f, 0.01f) * 0.35f;
			scale3(texel->color, 1.0f - circle * 0.2f + flower * 0.1f);
		}
		else if (cell == HOUSE_CELL_POST_HEAD)
		{
			float chevron = fabsf(fmodf(t * 6.0f + fabsf(s - 0.5f) * 2.0f, 1.0f) - 0.5f);
			float shield = inside_box(s, t, 0.35f, 0.3f, 0.65f, 0.55f, 0.01f) * (t < 0.45f ? 1.0f : inside_disc(s, t, 0.5f, 0.45f, 0.15f, 0.01f));

			texel->height += (chevron < 0.15f ? 0.25f : 0.0f) * (1.0f - shield) + shield * 0.3f;
			scale3(texel->color, 1.0f - (chevron < 0.15f ? 0.15f : 0.0f) * (1.0f - shield));
		}
		else
		{
			// Steps across, each lit on top and shaded under
			float step = fmodf(t * 4.0f, 1.0f);

			texel->height = 0.3f + 0.6f * (1.0f - t) + step * 0.1f;
			scale3(texel->color, 0.75f + 0.35f * (1.0f - step));
		}
		break;
	}
	case HOUSE_CELL_LANTERN:
	{
		// A six sided lantern: a dark frame, horn panes glowing, a ring on top
		float body = inside_box(s, t, 0.3f, 0.3f, 0.7f, 0.8f, 0.01f);
		float roof = (t > 0.18f && t < 0.3f && fabsf(s - 0.5f) < (t - 0.1f) * 1.3f) ? 1.0f : 0.0f;
		float ring = inside_disc(s, t, 0.5f, 0.14f, 0.05f, 0.006f) - inside_disc(s, t, 0.5f, 0.14f, 0.03f, 0.006f);
		float foot = inside_box(s, t, 0.4f, 0.8f, 0.6f, 0.86f, 0.01f);
		float pane = inside_box(s, t, 0.34f, 0.36f, 0.47f, 0.76f, 0.005f) + inside_box(s, t, 0.53f, 0.36f, 0.66f, 0.76f, 0.005f);
		float warm[3];

		iron_texel(x, y, texel);
		texel->alpha = clampf(body + roof + ring + foot, 0.0f, 1.0f);
		set3(warm, 0.9f, 0.7f, 0.4f);
		if (pane > 0.0f)
		{
			float flicker = 0.7f + 0.3f * fractal_of(x * 20.0f, y * 20.0f, 2, 3u);

			mix3(texel->color, texel->color, warm, pane * 0.8f);
			texel->roughness = mixf(texel->roughness, 0.4f, pane);
			texel->metal = mixf(texel->metal, 0.0f, pane);
			set3(texel->glow, warm[0] * flicker, warm[1] * flicker, warm[2] * flicker);
			scale3(texel->glow, pane * 1.2f);
		}
		break;
	}
	case HOUSE_CELL_FLOWER_BOX:
	{
		float box = inside_box(s, t, 0.02f, 0.55f, 0.98f, 0.98f, 0.005f);
		float greens = 0.0f;
		float reds = 0.0f;

		planks_texel(s, t * 1.4f - 0.6f, wide, high * 0.5f, 5, NULL, 0, 0.0f, 0.4f, texel);
		texel->alpha = box;
		for (int i = 0; i < 12; i++)
		{
			float cs = 0.08f + 0.84f * hash_unit_of(i, 0, 3u);
			float ct = 0.35f + 0.3f * hash_unit_of(i, 1, 3u);
			float leaf = inside_disc(s, t, cs, ct, 0.07f + 0.05f * hash_unit_of(i, 2, 3u), 0.02f);

			greens = greens > leaf ? greens : leaf;
		}
		for (int i = 0; i < 9; i++)
		{
			float cs = 0.1f + 0.8f * hash_unit_of(i, 0, 9u);
			float ct = 0.25f + 0.3f * hash_unit_of(i, 1, 9u);
			float bloom = inside_disc(s, t, cs, ct, 0.04f, 0.01f);

			reds = reds > bloom ? reds : bloom;
		}
		if (greens > 0.0f)
		{
			float green[3];

			set3(green, 0.15f + 0.1f * fractal_of(x * 30.0f, y * 30.0f, 2, 4u), 0.35f, 0.1f);
			mix3(texel->color, texel->color, green, greens);
			texel->height = mixf(texel->height, 0.6f, greens);
			texel->roughness = mixf(texel->roughness, 0.7f, greens);
			texel->alpha = texel->alpha > greens ? texel->alpha : greens;
		}
		if (reds > 0.0f)
		{
			float red[3];

			set3(red, 0.75f, 0.1f, 0.12f);
			if (hash_unit_of((int)(s * 20.0f), 0, 9u) > 0.6f)
			{
				set3(red, 0.9f, 0.88f, 0.85f);
			}
			mix3(texel->color, texel->color, red, reds);
			texel->height = mixf(texel->height, 0.7f, reds);
			texel->alpha = texel->alpha > reds ? texel->alpha : reds;
		}
		break;
	}
	case HOUSE_CELL_PLAQUE:
	{
		float border = inside_box(s, t, 0.08f, 0.08f, 0.92f, 0.92f, 0.005f) - inside_box(s, t, 0.14f, 0.14f, 0.86f, 0.86f, 0.005f);
		float one = inside_box(s, t, 0.36f, 0.3f, 0.44f, 0.7f, 0.005f);
		float seven = inside_box(s, t, 0.52f, 0.3f, 0.7f, 0.37f, 0.005f) + along_segment(s, t, 0.68f, 0.32f, 0.56f, 0.7f, 0.035f);
		float carved = clampf(border + one + seven, 0.0f, 1.0f);

		stone_texel(x, y, texel);
		texel->height -= carved * 0.3f;
		scale3(texel->color, 1.0f - carved * 0.35f);
		break;
	}
	case HOUSE_CELL_SUNDIAL:
	{
		float disc = inside_disc(s, t, 0.5f, 0.5f, 0.47f, 0.006f);
		float angle = atan2f(t - 0.5f, s - 0.5f);
		float d = sqrtf((s - 0.5f) * (s - 0.5f) + (t - 0.5f) * (t - 0.5f));
		float lines = 0.0f;
		float gnomon = along_segment(s, t, 0.5f, 0.5f, 0.5f, 0.18f, 0.012f);
		float shadow = along_segment(s, t, 0.5f, 0.5f, 0.3f, 0.3f, 0.016f) * 0.5f;

		stone_texel(x, y, texel);
		texel->alpha = disc;
		for (int i = 0; i < 12; i++)
		{
			float at = -3.14159f + 6.2832f * (float)i / 12.0f;
			float difference = fabsf(angle - at);

			difference = difference > 3.14159f ? 6.2832f - difference : difference;
			if (d > 0.2f && d < 0.42f && difference * d < 0.006f)
			{
				lines = 1.0f;
			}
		}
		lines = lines > (inside_disc(s, t, 0.5f, 0.5f, 0.44f, 0.004f) - inside_disc(s, t, 0.5f, 0.5f, 0.42f, 0.004f)) ? lines
			: (inside_disc(s, t, 0.5f, 0.5f, 0.44f, 0.004f) - inside_disc(s, t, 0.5f, 0.5f, 0.42f, 0.004f));
		texel->height -= lines * 0.25f;
		scale3(texel->color, 1.0f - lines * 0.4f - shadow);
		if (gnomon > 0.0f)
		{
			texel_t iron;

			iron_texel(x, y, &iron);
			iron.height = 0.98f;
			paint_over(texel, &iron, gnomon);
		}
		break;
	}
	case HOUSE_CELL_HORSESHOE:
	{
		float outer = inside_disc(s, t, 0.5f, 0.45f, 0.4f, 0.006f);
		float inner = inside_disc(s, t, 0.5f, 0.45f, 0.26f, 0.006f);
		float shoe = (outer - inner) * (t > 0.5f || fabsf(s - 0.5f) > 0.26f ? 1.0f : 0.0f);

		iron_texel(x, y, texel);
		texel->alpha = shoe;
		for (int i = 0; i < 6; i++)
		{
			float at = 0.3f + 2.5f * (float)i / 5.0f;
			float hole = inside_disc(s, t, 0.5f + cosf(at) * 0.33f, 0.45f + sinf(at) * 0.33f, 0.02f, 0.005f);

			texel->height -= hole * 0.4f;
			scale3(texel->color, 1.0f - hole * 0.5f);
		}
		break;
	}
	case HOUSE_CELL_HOOK_ROPE:
	{
		float hook = along_segment(s, t, 0.5f, 0.05f, 0.5f, 0.35f, 0.02f) + (inside_disc(s, t, 0.5f, 0.42f, 0.1f, 0.006f) - inside_disc(s, t, 0.5f, 0.42f, 0.06f, 0.006f)) * (t > 0.42f ? 1.0f : 0.0f);
		float coil = inside_disc(s, t, 0.5f, 0.62f, 0.3f, 0.01f) - inside_disc(s, t, 0.5f, 0.62f, 0.1f, 0.01f);

		iron_texel(x, y, texel);
		texel->alpha = clampf(hook, 0.0f, 1.0f);
		if (coil > 0.0f)
		{
			float d = sqrtf((s - 0.5f) * (s - 0.5f) + (t - 0.62f) * (t - 0.62f));
			float strands = 0.5f + 0.5f * sinf(d * 300.0f + atan2f(t - 0.62f, s - 0.5f) * 3.0f);
			float hemp[3];

			set3(hemp, 0.6f * (0.8f + 0.3f * strands), 0.5f * (0.8f + 0.3f * strands), 0.3f);
			mix3(texel->color, texel->color, hemp, coil);
			texel->height = mixf(texel->height, 0.5f + strands * 0.2f, coil);
			texel->roughness = mixf(texel->roughness, 0.9f, coil);
			texel->metal = mixf(texel->metal, 0.0f, coil);
			texel->alpha = texel->alpha > coil ? texel->alpha : coil;
		}
		break;
	}
	case HOUSE_CELL_SIGN_INN:
	case HOUSE_CELL_SIGN_SMITH:
	case HOUSE_CELL_SIGN_BAKER:
	case HOUSE_CELL_SIGN_TAILOR:
	case HOUSE_CELL_SIGN_HERBALIST:
	case HOUSE_CELL_SIGN_BOAR:
	case HOUSE_CELL_SIGN_BLANK:
		sign_board(s, t, wide, high, texel);
		if (cell != HOUSE_CELL_SIGN_BLANK && texel->alpha > 0.5f)
		{
			float alpha = texel->alpha;

			paint_sign_symbol(cell, s, t, wide, high, texel);
			texel->alpha = alpha;
		}
		break;
	default:
		// A free cell: flat grey, a cross through it, nothing to see
		set3(texel->color, 0.45f, 0.45f, 0.45f);
		texel->alpha = 1.0f;
		texel->height = 0.5f;
		texel->roughness = 0.8f;
		texel->metal = 0.0f;
		set3(texel->glow, 0.0f, 0.0f, 0.0f);
		if (along_segment(s, t, 0.0f, 0.0f, 1.0f, 1.0f, 0.01f) + along_segment(s, t, 1.0f, 0.0f, 0.0f, 1.0f, 0.01f) > 0.5f)
		{
			scale3(texel->color, 0.7f);
		}
		break;
	}
}

// ---------------------------------------------------------------------------
// Writing the pictures

static float to_byte_of(float value)
{
	return clampf(value * 255.0f + 0.5f, 0.0f, 255.0f);
}

static void write_bytes(void *context, void *data, int size)
{
	fwrite(data, 1, (size_t)size, (FILE *)context);
}

static bool write_png_file(const char *path, const uint8_t *pixels, int width, int height)
{
	FILE *file = fopen(path, "wb");
	int ok;

	if (file == NULL)
	{
		return false;
	}
	ok = stbi_write_png_to_func(write_bytes, file, width, height, 4, pixels, width * 4);
	fclose(file);
	return ok != 0;
}

static bool exists(const char *path)
{
	struct stat info;

	return stat(path, &info) == 0;
}

static void folders_for(const char *path)
{
	char copy[HOUSE_PATH_LENGTH * 2];

	snprintf(copy, sizeof(copy), "%s", path);
	for (char *at = copy + 1; *at != '\0'; at++)
	{
		if (*at == '/')
		{
			*at = '\0';
			mkdir(copy, 0755);
			*at = '/';
		}
	}
}

bool house_atlas_stand_in(const char *data_directory, bool remake, char names_out[4][HOUSE_PATH_LENGTH])
{
	static const char *const suffixes[4] = { "color", "normal", "arm", "emissive" };
	char paths[4][HOUSE_PATH_LENGTH * 2];
	bool missing = remake;
	int size = ATLAS_SIZE;
	uint8_t *maps[4];
	float *heights;
	bool ok = true;

	for (int m = 0; m < 4; m++)
	{
		snprintf(names_out[m], HOUSE_PATH_LENGTH, "props/textures/house_details_%s.png", suffixes[m]);
		snprintf(paths[m], sizeof(paths[m]), "%s/%s", data_directory, names_out[m]);
		missing = missing || !exists(paths[m]);
	}
	if (!missing)
	{
		return true;
	}
	folders_for(paths[0]);
	heights = malloc(sizeof(float) * (size_t)size * (size_t)size);
	for (int m = 0; m < 4; m++)
	{
		maps[m] = malloc((size_t)size * (size_t)size * 4);
	}
	if (heights == NULL || maps[0] == NULL || maps[1] == NULL || maps[2] == NULL || maps[3] == NULL)
	{
		free(heights);
		for (int m = 0; m < 4; m++)
		{
			free(maps[m]);
		}
		return false;
	}
	// The gaps between cells (there are none, the cells tile the picture)
	// and anything unpainted: grey
	memset(maps[0], 110, (size_t)size * (size_t)size * 4);
	memset(maps[3], 0, (size_t)size * (size_t)size * 4);
	for (size_t i = 0; i < (size_t)size * (size_t)size; i++)
	{
		heights[i] = 0.5f;
		maps[2][i * 4] = 255;
		maps[2][i * 4 + 1] = 200;
		maps[2][i * 4 + 2] = 0;
		maps[2][i * 4 + 3] = 255;
		maps[3][i * 4 + 3] = 255;
	}
	for (int cell = 0; cell < HOUSE_CELL_COUNT; cell++)
	{
		const atlas_cell_t *c = &cells[cell];

		for (int y = 0; y < c->height; y++)
		{
			for (int x = 0; x < c->width; x++)
			{
				size_t here = (size_t)(c->y + y) * (size_t)size + (size_t)(c->x + x);
				texel_t texel;

				cell_texel(cell, ((float)x + 0.5f) / (float)c->width, ((float)y + 0.5f) / (float)c->height, &texel);
				for (int k = 0; k < 3; k++)
				{
					maps[0][here * 4 + k] = (uint8_t)to_byte_of(clampf(texel.color[k], 0.0f, 1.0f));
					maps[3][here * 4 + k] = (uint8_t)to_byte_of(clampf(texel.glow[k], 0.0f, 1.0f));
				}
				maps[0][here * 4 + 3] = (uint8_t)to_byte_of(texel.alpha);
				heights[here] = texel.height;
				maps[2][here * 4] = 255;
				maps[2][here * 4 + 1] = (uint8_t)to_byte_of(texel.roughness);
				maps[2][here * 4 + 2] = (uint8_t)to_byte_of(texel.metal);
			}
		}
		// The normals from the heights, within the cell (not across its edge)
		for (int y = 0; y < c->height; y++)
		{
			for (int x = 0; x < c->width; x++)
			{
				int left = x > 0 ? x - 1 : x;
				int right = x < c->width - 1 ? x + 1 : x;
				int up = y > 0 ? y - 1 : y;
				int down = y < c->height - 1 ? y + 1 : y;
				size_t here = (size_t)(c->y + y) * (size_t)size + (size_t)(c->x + x);
				float hl = heights[(size_t)(c->y + y) * (size_t)size + (size_t)(c->x + left)];
				float hr = heights[(size_t)(c->y + y) * (size_t)size + (size_t)(c->x + right)];
				float hu = heights[(size_t)(c->y + up) * (size_t)size + (size_t)(c->x + x)];
				float hd = heights[(size_t)(c->y + down) * (size_t)size + (size_t)(c->x + x)];
				float strength = 2.5f;
				float nx = (hl - hr) * strength;
				float ny = (hd - hu) * strength;
				float length = sqrtf(nx * nx + ny * ny + 1.0f);
				float around = 0.0f;
				float occlusion;

				for (int r = 1; r <= 3; r++)
				{
					int step = r * 2;
					int xl = x - step < 0 ? 0 : x - step;
					int xr = x + step >= c->width ? c->width - 1 : x + step;
					int yu = y - step < 0 ? 0 : y - step;
					int yd = y + step >= c->height ? c->height - 1 : y + step;

					around += heights[(size_t)(c->y + y) * (size_t)size + (size_t)(c->x + xl)];
					around += heights[(size_t)(c->y + y) * (size_t)size + (size_t)(c->x + xr)];
					around += heights[(size_t)(c->y + yu) * (size_t)size + (size_t)(c->x + x)];
					around += heights[(size_t)(c->y + yd) * (size_t)size + (size_t)(c->x + x)];
				}
				around /= 12.0f;
				occlusion = clampf(1.0f - (around - heights[here]) * 2.0f, 0.5f, 1.0f);
				maps[1][here * 4] = (uint8_t)to_byte_of(nx / length * 0.5f + 0.5f);
				maps[1][here * 4 + 1] = (uint8_t)to_byte_of(ny / length * 0.5f + 0.5f);
				maps[1][here * 4 + 2] = (uint8_t)to_byte_of(1.0f / length * 0.5f + 0.5f);
				maps[1][here * 4 + 3] = 255;
				maps[2][here * 4] = (uint8_t)to_byte_of(occlusion);
				maps[2][here * 4 + 3] = 255;
			}
		}
	}
	for (int m = 0; m < 4; m++)
	{
		ok = write_png_file(paths[m], maps[m], size, size) && ok;
	}
	free(heights);
	for (int m = 0; m < 4; m++)
	{
		free(maps[m]);
	}
	return ok;
}

// ---------------------------------------------------------------------------
// The template for the painter: the cells framed and labeled, and a guide

// A 5 by 7 font for the labels: digits, capitals, a few marks
static const char *const glyph_rows[][7] =
{
	{ "01110", "10001", "10011", "10101", "11001", "10001", "01110" }, // 0
	{ "00100", "01100", "00100", "00100", "00100", "00100", "01110" }, // 1
	{ "01110", "10001", "00001", "00010", "00100", "01000", "11111" }, // 2
	{ "11111", "00010", "00100", "00010", "00001", "10001", "01110" }, // 3
	{ "00010", "00110", "01010", "10010", "11111", "00010", "00010" }, // 4
	{ "11111", "10000", "11110", "00001", "00001", "10001", "01110" }, // 5
	{ "00110", "01000", "10000", "11110", "10001", "10001", "01110" }, // 6
	{ "11111", "00001", "00010", "00100", "01000", "01000", "01000" }, // 7
	{ "01110", "10001", "10001", "01110", "10001", "10001", "01110" }, // 8
	{ "01110", "10001", "10001", "01111", "00001", "00010", "01100" }, // 9
	{ "01110", "10001", "10001", "11111", "10001", "10001", "10001" }, // A
	{ "11110", "10001", "10001", "11110", "10001", "10001", "11110" }, // B
	{ "01110", "10001", "10000", "10000", "10000", "10001", "01110" }, // C
	{ "11100", "10010", "10001", "10001", "10001", "10010", "11100" }, // D
	{ "11111", "10000", "10000", "11110", "10000", "10000", "11111" }, // E
	{ "11111", "10000", "10000", "11110", "10000", "10000", "10000" }, // F
	{ "01110", "10001", "10000", "10111", "10001", "10001", "01111" }, // G
	{ "10001", "10001", "10001", "11111", "10001", "10001", "10001" }, // H
	{ "01110", "00100", "00100", "00100", "00100", "00100", "01110" }, // I
	{ "00111", "00010", "00010", "00010", "00010", "10010", "01100" }, // J
	{ "10001", "10010", "10100", "11000", "10100", "10010", "10001" }, // K
	{ "10000", "10000", "10000", "10000", "10000", "10000", "11111" }, // L
	{ "10001", "11011", "10101", "10101", "10001", "10001", "10001" }, // M
	{ "10001", "10001", "11001", "10101", "10011", "10001", "10001" }, // N
	{ "01110", "10001", "10001", "10001", "10001", "10001", "01110" }, // O
	{ "11110", "10001", "10001", "11110", "10000", "10000", "10000" }, // P
	{ "01110", "10001", "10001", "10001", "10101", "10010", "01101" }, // Q
	{ "11110", "10001", "10001", "11110", "10100", "10010", "10001" }, // R
	{ "01111", "10000", "10000", "01110", "00001", "00001", "11110" }, // S
	{ "11111", "00100", "00100", "00100", "00100", "00100", "00100" }, // T
	{ "10001", "10001", "10001", "10001", "10001", "10001", "01110" }, // U
	{ "10001", "10001", "10001", "10001", "10001", "01010", "00100" }, // V
	{ "10001", "10001", "10001", "10101", "10101", "10101", "01010" }, // W
	{ "10001", "10001", "01010", "00100", "01010", "10001", "10001" }, // X
	{ "10001", "10001", "01010", "00100", "00100", "00100", "00100" }, // Y
	{ "11111", "00001", "00010", "00100", "01000", "10000", "11111" }, // Z
	{ "00000", "00000", "00000", "00000", "00000", "00000", "11111" }, // _
	{ "00000", "00000", "00000", "00000", "00000", "00000", "00000" }, // space
	{ "00000", "00000", "00000", "00000", "00000", "01100", "01100" }, // .
};

static int glyph_index(char c)
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	if (c >= 'a' && c <= 'z')
	{
		c = (char)(c - 'a' + 'A');
	}
	if (c >= 'A' && c <= 'Z')
	{
		return 10 + c - 'A';
	}
	return c == '_' ? 36 : (c == '.' ? 38 : 37);
}

static void put_pixel(uint8_t *pixels, int size, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	if (x >= 0 && y >= 0 && x < size && y < size)
	{
		uint8_t *p = &pixels[((size_t)y * (size_t)size + (size_t)x) * 4];

		p[0] = r;
		p[1] = g;
		p[2] = b;
		p[3] = 255;
	}
}

static void draw_text(uint8_t *pixels, int size, int x, int y, int scale, const char *text, uint8_t r, uint8_t g, uint8_t b)
{
	for (const char *at = text; *at != '\0'; at++)
	{
		const char *const *rows = glyph_rows[glyph_index(*at)];

		for (int row = 0; row < 7; row++)
		{
			for (int column = 0; column < 5; column++)
			{
				if (rows[row][column] == '1')
				{
					for (int dy = 0; dy < scale; dy++)
					{
						for (int dx = 0; dx < scale; dx++)
						{
							put_pixel(pixels, size, x + column * scale + dx, y + row * scale + dy, r, g, b);
						}
					}
				}
			}
		}
		x += 6 * scale;
	}
}

bool house_atlas_template(const char *png_path, const char *guide_path)
{
	int size = ATLAS_SIZE;
	uint8_t *pixels = malloc((size_t)size * (size_t)size * 4);
	FILE *guide;
	bool ok;

	if (pixels == NULL)
	{
		return false;
	}
	// A faint checker over everything (the painter sees the texel scale),
	// each cell framed in red, its index and name in its top left corner,
	// its size in meters under them
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			uint8_t grey = ((x / 32 + y / 32) % 2 == 0) ? 200 : 185;

			put_pixel(pixels, size, x, y, grey, grey, grey);
		}
	}
	for (int cell = 0; cell < HOUSE_CELL_COUNT; cell++)
	{
		const atlas_cell_t *c = &cells[cell];
		char label[96];

		for (int x = c->x; x < c->x + c->width; x++)
		{
			for (int k = 0; k < 2; k++)
			{
				put_pixel(pixels, size, x, c->y + k, 200, 30, 30);
				put_pixel(pixels, size, x, c->y + c->height - 1 - k, 200, 30, 30);
			}
		}
		for (int y = c->y; y < c->y + c->height; y++)
		{
			for (int k = 0; k < 2; k++)
			{
				put_pixel(pixels, size, c->x + k, y, 200, 30, 30);
				put_pixel(pixels, size, c->x + c->width - 1 - k, y, 200, 30, 30);
			}
		}
		snprintf(label, sizeof(label), "%d", cell);
		draw_text(pixels, size, c->x + 8, c->y + 8, 4, label, 160, 20, 20);
		draw_text(pixels, size, c->x + 8, c->y + 40, 2, c->name, 40, 40, 40);
		snprintf(label, sizeof(label), "%.2fX%.2fM", (double)c->meters_wide, (double)c->meters_high);
		draw_text(pixels, size, c->x + 8, c->y + 58, 2, label, 90, 90, 90);
	}
	folders_for(png_path);
	ok = write_png_file(png_path, pixels, size, size);
	free(pixels);
	guide = fopen(guide_path, "w");
	if (guide == NULL)
	{
		return false;
	}
	fprintf(guide, "# The houses' details atlas: what to paint\n\n");
	fprintf(guide, "One square picture, %d by %d pixels, cut into cells (see house_details_template.png: each cell is framed\n", size, size);
	fprintf(guide, "in red, with its number and name in its corner). Every cell is a separate small picture of one thing a medieval\n");
	fprintf(guide, "house wears; the game maps a window light, a shutter, the end of a beam or a hanging sign onto its cell, so each\n");
	fprintf(guide, "cell must be painted straight on (orthographic, seen square from the front), filling the whole cell edge to edge,\n");
	fprintf(guide, "in flat, even, shadowless daylight (the game lights it; do not paint a sun's shadow or highlights), with nothing\n");
	fprintf(guide, "of the surroundings (no wall round a window, no frame round a door leaf: only the thing itself).\n\n");
	fprintf(guide, "Style: a realistic, hand painted, weathered look, muted colors, the kind of thing found on a half timbered house\n");
	fprintf(guide, "in an old town: oak gone grey, iron gone rusty, lime wash, old glass. Nothing cartoonish, no outlines. The\n");
	fprintf(guide, "pixel scale is the same in every cell: 256 pixels are half a meter (so a window light is 0.5 by 1 m, a door\n");
	fprintf(guide, "leaf 1 by 2 m, a sign 1 by 0.5 m, a joist's end a quarter of a meter), and details should be sized accordingly.\n\n");
	fprintf(guide, "Transparency: where a cell says so (lantern, horseshoe, signs, hook, flower box, sundial, the heart in the\n");
	fprintf(guide, "shutter), the picture has a shape, and everything outside the shape is fully transparent (alpha 0); keep a\n");
	fprintf(guide, "clean hard edge. Elsewhere the cell is fully opaque. The glass of the windows is painted as seen by day;\n");
	fprintf(guide, "the game makes it glow at night by itself.\n\n");
	fprintf(guide, "Deliver the picture as a PNG with alpha, %d by %d, no compression artifacts, the cells exactly where the\n", size, size);
	fprintf(guide, "template puts them (paint over a copy of the template, then remove its red frames and labels).\n\n");
	fprintf(guide, "## The cells\n\n");
	fprintf(guide, "| # | name | pixels (x, y, width, height) | meters | what to paint |\n|---|---|---|---|---|\n");
	for (int cell = 0; cell < HOUSE_CELL_COUNT; cell++)
	{
		const atlas_cell_t *c = &cells[cell];

		fprintf(guide, "| %d | %s | %d, %d, %d, %d | %.2f x %.2f | %s |\n", cell, c->name, c->x, c->y, c->width, c->height, (double)c->meters_wide,
			(double)c->meters_high, c->brief);
	}
	fprintf(guide, "\n## Prompt to start from\n\n");
	fprintf(guide, "\"Paint a texture atlas for a video game, %d by %d pixels, following the attached template exactly: it is divided\n", size, size);
	fprintf(guide, "into cells framed in red, and each cell is one separate object seen straight from the front, filling its cell\n");
	fprintf(guide, "edge to edge, in flat shadowless light, realistic and weathered, medieval, muted colors, no outlines, no text.\n");
	fprintf(guide, "Remove the red frames and labels. Where an object has a shape (a lantern, a horseshoe, a sign board) the\n");
	fprintf(guide, "background is transparent. Cell by cell: ...\" followed by the table above.\n");
	fclose(guide);
	return ok;
}
