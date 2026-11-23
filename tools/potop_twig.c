// potop_twig: draws a conifer's twig from nothing, the picture the plant
// grower lays on its twig cards (a recipe's twig = ...): the twig upright,
// its cut foot at the bottom in the middle, on a clear background (alpha), a
// picture twice as tall as wide. A desktop tool: it is not part of the game
// and never ships with it.
//
//   potop_twig [--width W] [--seed N] KIND OUT.png
//
// Kinds:
//   pine     a mountain pine's (jneapăn): a stout shoot, needles in pairs,
//            long, stiff, a little curved, pointing forward, crowded towards
//            the tip into a brush round its bud; a side shoot or two
//   juniper  a juniper's: a branchy twig, needles in threes, short, sharp,
//            spreading, each with a pale stripe along it; blue green
//
// It is drawn three times as big and averaged down (the edges smooth), then
// the color spread out under the clear background (no dark halos when the
// picture is made smaller).

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image_write.h"

#define SUPERSAMPLE 3
#define PI_F 3.14159265f

typedef struct
{
	int width;                     // the drawing's (SUPERSAMPLE times the picture's)
	int height;
	float *pixels;                 // RGBA, premultiplied
	float scale;                   // pixels a unit (the picture's width is one unit)
	float tall;                    // its height, units
	float fit;                     // the drawing shrunk this much round the foot to fit (1: as drawn)
	float foot_x;                  // (that foot, units)
	float foot_y;
} canvas_t;

typedef struct
{
	uint64_t state;
} random_t;

static uint32_t random_next(random_t *random)
{
	random->state = random->state * 6364136223846793005ull + 1442695040888963407ull;
	return (uint32_t)(random->state >> 33);
}

static float random_float(random_t *random)
{
	return (float)(random_next(random) & 0xFFFFFF) / 16777216.0f;
}

static float random_range(random_t *random, float low, float high)
{
	return low + (high - low) * random_float(random);
}

static float clampf(float value, float low, float high)
{
	return value < low ? low : (value > high ? high : value);
}

static float mixf(float a, float b, float t)
{
	return a + (b - a) * t;
}

// --- Drawing --------------------------------------------------------------------------

// A color with how it is lit across a round piece: shade 0 at the edge in
// shadow, 1 at the lit edge
typedef struct
{
	float dark[3];
	float light[3];
	float stripe[3];               // a pale line along the middle (juniper), if stripe_width > 0
	float stripe_width;            // of the half width
	float tip[3];                  // towards the far end
	float tip_from;                // along 0..1, the tip color from here
	float rough;                   // bark: its scales and cracks (0: smooth)
	float brightness;              // all of it this much brighter or darker (1: as it is; 0 counts as 1)
} paint_t;

// Value noise over the drawing's pixels (bark): smooth, 0..1
static float hash_unit(int x, int y)
{
	uint32_t value = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u;

	value ^= value >> 13;
	value *= 0x5BD1E995u;
	value ^= value >> 15;
	return (float)(value & 0xFFFF) / 65535.0f;
}

static float value_noise(float x, float y)
{
	int ix = (int)floorf(x);
	int iy = (int)floorf(y);
	float fx = x - (float)ix;
	float fy = y - (float)iy;
	float a = hash_unit(ix, iy);
	float b = hash_unit(ix + 1, iy);
	float c = hash_unit(ix, iy + 1);
	float d = hash_unit(ix + 1, iy + 1);

	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	return mixf(mixf(a, b, fx), mixf(c, d, fx), fy);
}

// One piece of a needle or a stem: from a (radius ra) to b (radius rb), as
// far along the whole as along_a .. along_b; lit from the upper left
static void draw_segment(canvas_t *canvas, float ax, float ay, float ra, float bx, float by, float rb, float along_a, float along_b,
	const paint_t *paint)
{
	float s = canvas->scale * canvas->fit;
	float x0 = (canvas->foot_x + (ax - canvas->foot_x) * canvas->fit) * canvas->scale;
	float y0 = (canvas->foot_y + (ay - canvas->foot_y) * canvas->fit) * canvas->scale;
	float x1 = (canvas->foot_x + (bx - canvas->foot_x) * canvas->fit) * canvas->scale;
	float y1 = (canvas->foot_y + (by - canvas->foot_y) * canvas->fit) * canvas->scale;
	float r0 = ra * s;
	float r1 = rb * s;
	float dx = x1 - x0;
	float dy = y1 - y0;
	float length_squared = dx * dx + dy * dy;
	float rmax = fmaxf(r0, r1) + 1.0f;
	int left = (int)floorf(fminf(x0, x1) - rmax);
	int right = (int)ceilf(fmaxf(x0, x1) + rmax);
	int top = (int)floorf(fminf(y0, y1) - rmax);
	int bottom = (int)ceilf(fmaxf(y0, y1) + rmax);
	// (across: the side the light comes from, the upper left)
	float normal_x = length_squared > 0.0f ? -dy / sqrtf(length_squared) : 0.0f;
	float normal_y = length_squared > 0.0f ? dx / sqrtf(length_squared) : 1.0f;
	float light_side = -0.7f * normal_x - 0.7f * normal_y;

	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right >= canvas->width ? canvas->width - 1 : right;
	bottom = bottom >= canvas->height ? canvas->height - 1 : bottom;
	for (int y = top; y <= bottom; y++)
	{
		for (int x = left; x <= right; x++)
		{
			float px = (float)x + 0.5f - x0;
			float py = (float)y + 0.5f - y0;
			float t = length_squared > 0.0f ? clampf((px * dx + py * dy) / length_squared, 0.0f, 1.0f) : 0.0f;
			float cx = px - t * dx;
			float cy = py - t * dy;
			float distance = sqrtf(cx * cx + cy * cy);
			float radius = mixf(r0, r1, t);
			float coverage = clampf(radius - distance + 0.5f, 0.0f, 1.0f);
			float across;
			float shade;
			float along;
			float color[3];
			float *pixel;

			if (coverage <= 0.0f || radius <= 0.0f)
			{
				continue;
			}
			// (-1 .. 1 across the piece, signed towards the light)
			across = clampf((cx * normal_x + cy * normal_y) / fmaxf(radius, 0.5f), -1.0f, 1.0f);
			shade = 0.5f + 0.5f * across * (light_side >= 0.0f ? 1.0f : -1.0f);
			// (round: the middle bulges towards the viewer, lit)
			shade = clampf(0.35f + 0.65f * shade * (0.6f + 0.4f * sqrtf(fmaxf(1.0f - across * across, 0.0f))), 0.0f, 1.0f);
			along = mixf(along_a, along_b, t);
			for (int k = 0; k < 3; k++)
			{
				color[k] = mixf(paint->dark[k], paint->light[k], shade);
				if (paint->tip_from < 1.0f && along > paint->tip_from)
				{
					color[k] = mixf(color[k], paint->tip[k], clampf((along - paint->tip_from) / (1.0f - paint->tip_from), 0.0f, 1.0f));
				}
				if (paint->stripe_width > 0.0f && fabsf(across) < paint->stripe_width)
				{
					color[k] = mixf(color[k], paint->stripe[k], 0.75f * (1.0f - fabsf(across) / paint->stripe_width));
				}
			}
			if (paint->rough > 0.0f)
			{
				// (scales: blocks along the piece, dark cracks between)
				float block = value_noise((float)x / 7.0f, (float)y / 7.0f) * 0.6f + value_noise((float)x / 2.5f, (float)y / 2.5f) * 0.4f;
				float crack = 1.0f - 0.7f * powf(clampf(1.0f - fabsf(block - 0.5f) * 6.0f, 0.0f, 1.0f), 2.0f);

				for (int k = 0; k < 3; k++)
				{
					color[k] *= mixf(1.0f, (0.65f + 0.7f * block) * crack, paint->rough);
				}
			}
			if (paint->brightness > 0.0f)
			{
				for (int k = 0; k < 3; k++)
				{
					color[k] *= paint->brightness;
				}
			}
			pixel = &canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4];
			for (int k = 0; k < 3; k++)
			{
				pixel[k] = color[k] * coverage + pixel[k] * (1.0f - coverage);
			}
			pixel[3] = coverage + pixel[3] * (1.0f - coverage);
		}
	}
}

// A curved piece (a quadratic curve from a through the control c to b), its
// radius from ra to rb, in pieces
static void draw_curve(canvas_t *canvas, float ax, float ay, float cx, float cy, float bx, float by, float ra, float rb, const paint_t *paint)
{
	const int pieces = 10;
	float last_x = ax;
	float last_y = ay;

	for (int i = 1; i <= pieces; i++)
	{
		float t = (float)i / (float)pieces;
		float u = 1.0f - t;
		float x = u * u * ax + 2.0f * u * t * cx + t * t * bx;
		float y = u * u * ay + 2.0f * u * t * cy + t * t * by;
		float t0 = (float)(i - 1) / (float)pieces;

		draw_segment(canvas, last_x, last_y, mixf(ra, rb, t0), x, y, mixf(ra, rb, t), t0, t, paint);
		last_x = x;
		last_y = y;
	}
}

// --- A shoot ---------------------------------------------------------------------------

typedef struct
{
	float fx;
	float fy;
	float angle;
	float bend;
	float length;
	float radius;
	float scale;                   // its needles' size, of the main shoot's
	float taper;                   // its radius at its tip, of its foot's (0 counts as 0.45)
	bool bare;                     // no needles on it (a branch carrying shoots)
	float needles_from;            // its needles from this far along (0: the style's)
	float crook;                   // how crooked (its heading wanders by up to this many radians)
	float phase;                   // (where its wander starts)
} shoot_t;

// A shoot's point and heading as far along it as t, its crooks in
static void shoot_point(const shoot_t *shoot, float t, float *x, float *y, float *heading)
{
	// (the crooks: the heading wanders, the point follows it, summed in
	// steps so it stays on the drawn line)
	const int steps = 32;
	float px = shoot->fx;
	float py = shoot->fy;
	float angle = shoot->angle;

	for (int i = 0; i < steps; i++)
	{
		float u = ((float)i + 0.5f) / (float)steps * t;

		angle = shoot->angle + shoot->bend * u + shoot->crook * (sinf(u * 11.0f + shoot->phase) * 0.6f + sinf(u * 23.0f + shoot->phase * 1.7f) * 0.4f);
		px += sinf(angle) * shoot->length * t / (float)steps;
		py -= cosf(angle) * shoot->length * t / (float)steps;
	}
	*x = px;
	*y = py;
	*heading = angle;
}

typedef struct
{
	float x;
	float y;
	float angle;                   // the needle's heading
	float length;
	float curl;                    // how far it curves (radians over it)
	float radius;
	float depth;                   // -1 behind the shoot .. 1 in front (drawn in this order, darker behind)
	float hue;                     // 0..1: which of the greens
	float brightness;
} needle_t;

static int compare_needles(const void *first, const void *second)
{
	float a = ((const needle_t *)first)->depth;
	float b = ((const needle_t *)second)->depth;

	return a < b ? -1 : (a > b ? 1 : 0);
}

typedef struct
{
	float length_low;              // a needle's length, units
	float length_high;
	float radius;                  // its half width at its foot
	float angle_low;               // from the shoot's heading, radians
	float angle_high;
	float curl;                    // towards the shoot's tip (radians over it, give or take half)
	int per_bundle;                // needles in each bundle (pairs, threes)
	float bundles_per_unit;        // along the shoot, at its foot ...
	float crowding;                // ... and this many times as many at its tip
	float bare;                    // the shoot's share at its foot with no needles
	float brush;                   // the tip's brush: needles round the bud, pointing on
	paint_t greens[2];             // the needles (picked between by each one's hue)
	float behind_dark;             // the needles behind the shoot this much darker
	float spread_taper;            // the needles near the shoot's tip spread this share as far as at the tuft's foot (0 counts as 1)
	float brightness_spread;       // each needle give or take this much brighter
} needle_style_t;

#define NEEDLES_MAX 20000

static int add_shoot_needles(needle_t *needles, int count, random_t *random, const needle_style_t *style_in, const shoot_t *shoot)
{
	float length = shoot->length;
	float scale = shoot->scale;
	float needles_from = shoot->needles_from;
	needle_style_t own = *style_in;
	const needle_style_t *style = &own;
	float t;

	if (needles_from > 0.0f)
	{
		own.bare = needles_from;
	}
	t = style->bare;

	while (t < 1.0f && count < NEEDLES_MAX - 16)
	{
		float density = style->bundles_per_unit * mixf(1.0f, style->crowding, t * t) / fmaxf(scale, 0.2f);
		float x;
		float y;
		float heading;

		shoot_point(shoot, t, &x, &y, &heading);
		for (int k = 0; k < style->per_bundle; k++)
		{
			needle_t *needle = &needles[count++];
			// (a bundle's needles round the shoot: seen from the side,
			// some to the left, some to the right, some towards us)
			float around = random_range(random, 0.0f, 2.0f * PI_F) + (float)k * 2.0f * PI_F / (float)style->per_bundle;
			float side = sinf(around);
			float tuft = clampf((t - style->bare) / fmaxf(1.0f - style->bare, 0.01f), 0.0f, 1.0f);
			float spread = random_range(random, style->angle_low, style->angle_high) * (0.35f + 0.65f * fabsf(side))
				* mixf(1.0f, style->spread_taper > 0.0f ? style->spread_taper : 1.0f, tuft);

			needle->x = x + random_range(random, -0.004f, 0.004f);
			needle->y = y;
			needle->angle = heading + (side >= 0.0f ? spread : -spread);
			needle->length = random_range(random, style->length_low, style->length_high) * scale * mixf(0.8f, 1.05f, t);
			needle->curl = (side >= 0.0f ? -1.0f : 1.0f) * style->curl * random_range(random, 0.5f, 1.5f);
			needle->radius = style->radius * scale * random_range(random, 0.85f, 1.15f);
			needle->depth = cosf(around);
			needle->hue = random_float(random);
			needle->brightness = 1.0f + random_range(random, -style->brightness_spread, style->brightness_spread);
		}
		t += 1.0f / fmaxf(density * length, 1.0f);
	}
	// The brush round the bud: needles from the last stretch, pointing on
	for (int i = 0; i < (int)(style->brush * 20.0f) && count < NEEDLES_MAX; i++)
	{
		needle_t *needle = &needles[count++];
		float along = random_range(random, 0.9f, 1.0f);
		float x;
		float y;
		float heading;
		float around = random_range(random, 0.0f, 2.0f * PI_F);

		shoot_point(shoot, along, &x, &y, &heading);
		needle->x = x;
		needle->y = y;
		needle->angle = heading + random_range(random, -0.55f, 0.55f) * fabsf(sinf(around));
		needle->length = random_range(random, style->length_low, style->length_high) * scale * 0.9f;
		needle->curl = random_range(random, -style->curl, style->curl);
		needle->radius = style->radius * scale;
		needle->depth = cosf(around);
		needle->hue = random_float(random);
		needle->brightness = 1.0f + random_range(random, -style->brightness_spread, style->brightness_spread);
	}
	return count;
}

static void draw_needles(canvas_t *canvas, needle_t *needles, int count, const needle_style_t *style, bool front)
{
	for (int i = 0; i < count; i++)
	{
		const needle_t *needle = &needles[i];
		float end_x;
		float end_y;
		float control_x;
		float control_y;
		float middle_angle = needle->angle + needle->curl * 0.5f;
		float end_angle = needle->angle + needle->curl;
		paint_t paint;
		float darken;

		if ((needle->depth >= 0.0f) != front)
		{
			continue;
		}
		end_x = needle->x + (sinf(needle->angle) + sinf(end_angle)) * 0.5f * needle->length;
		end_y = needle->y - (cosf(needle->angle) + cosf(end_angle)) * 0.5f * needle->length;
		control_x = needle->x + sinf(middle_angle) * needle->length * 0.5f;
		control_y = needle->y - cosf(middle_angle) * needle->length * 0.5f;
		paint = style->greens[0];
		for (int k = 0; k < 3; k++)
		{
			paint.dark[k] = mixf(style->greens[0].dark[k], style->greens[1].dark[k], needle->hue);
			paint.light[k] = mixf(style->greens[0].light[k], style->greens[1].light[k], needle->hue);
		}
		// (behind the shoot: in its shade)
		darken = (1.0f - style->behind_dark * clampf(-needle->depth, 0.0f, 1.0f)) * needle->brightness;
		for (int k = 0; k < 3; k++)
		{
			paint.dark[k] *= darken;
			paint.light[k] *= darken;
			paint.tip[k] *= darken;
		}
		draw_curve(canvas, needle->x, needle->y, control_x, control_y, end_x, end_y, needle->radius, needle->radius * 0.15f, &paint);
	}
}

// --- The kinds -------------------------------------------------------------------------


static void draw_twig(canvas_t *canvas, random_t *random, const needle_style_t *style, const paint_t *bark, const paint_t *bud,
	const shoot_t *shoots, int shoot_count)
{
	needle_t *needles = malloc(sizeof(needle_t) * NEEDLES_MAX);
	int count = 0;

	if (needles == NULL)
	{
		return;
	}
	// The drawing shrunk round the first shoot's foot until all of it (its
	// needles' reach round each shoot too) is inside the picture
	{
		float reach = style->length_high * 1.1f;
		float half = 0.0f;
		float top = shoots[0].fy;

		for (int i = 0; i < shoot_count; i++)
		{
			for (int p = 0; p <= 16; p++)
			{
				float x;
				float y;
				float heading;

				shoot_point(&shoots[i], (float)p / 16.0f, &x, &y, &heading);
				half = fmaxf(half, fabsf(x - shoots[0].fx) + reach);
				top = fminf(top, y - reach);
			}
		}
		canvas->foot_x = shoots[0].fx;
		canvas->foot_y = shoots[0].fy;
		canvas->fit = fminf(1.0f, fminf(0.48f / fmaxf(half, 0.01f), (shoots[0].fy - 0.01f) / fmaxf(shoots[0].fy - top, 0.01f)));
	}
	for (int i = 0; i < shoot_count; i++)
	{
		if (shoots[i].bare)
		{
			continue;
		}
		count = add_shoot_needles(needles, count, random, style, &shoots[i]);
	}
	qsort(needles, (size_t)count, sizeof(needle_t), compare_needles);
	// Behind the shoots, the shoots, in front of them
	draw_needles(canvas, needles, count, style, false);
	for (int i = 0; i < shoot_count; i++)
	{
		const shoot_t *shoot = &shoots[i];
		const int pieces = 24;
		float last_x = shoot->fx;
		float last_y = shoot->fy;
		float heading;

		for (int p = 1; p <= pieces; p++)
		{
			float t = (float)p / (float)pieces;
			float t0 = (float)(p - 1) / (float)pieces;
			float x;
			float y;

			shoot_point(shoot, t, &x, &y, &heading);
			float taper = shoot->taper > 0.0f ? shoot->taper : 0.45f;

			draw_segment(canvas, last_x, last_y, shoot->radius * mixf(1.0f, taper, t0), x, y, shoot->radius * mixf(1.0f, taper, t), t0, t, bark);
			last_x = x;
			last_y = y;
		}
		// (its bud at the tip)
		if (bud != NULL && !shoot->bare)
		{
			draw_segment(canvas, last_x, last_y, shoot->radius * 0.9f, last_x + sinf(heading) * shoot->radius * 3.0f,
				last_y - cosf(heading) * shoot->radius * 3.0f, shoot->radius * 0.3f, 0.0f, 1.0f, bud);
		}
	}
	draw_needles(canvas, needles, count, style, true);
	free(needles);
}

// A mountain pine's branch tip: a stout stem out of the cut, forking into a
// few branches (and those once more), every shoot ending in a tuft of long
// needles in pairs round its bud, wide at the tuft's foot, closing towards
// the tip: brushes on bare scaly branches
static void draw_pine(canvas_t *canvas, random_t *random)
{
	needle_style_t style = {
		.length_low = 0.14f, .length_high = 0.2f, .radius = 0.0032f,
		.angle_low = 0.5f, .angle_high = 1.85f, .curl = 0.15f,
		.per_bundle = 2, .bundles_per_unit = 330.0f, .crowding = 1.6f, .bare = 0.45f, .brush = 2.6f,
		.greens = {
			{ .dark = { 0.06f, 0.10f, 0.05f }, .light = { 0.25f, 0.36f, 0.17f }, .tip = { 0.34f, 0.41f, 0.22f }, .tip_from = 0.85f },
			{ .dark = { 0.07f, 0.12f, 0.06f }, .light = { 0.30f, 0.41f, 0.20f }, .tip = { 0.39f, 0.45f, 0.25f }, .tip_from = 0.85f } },
		.behind_dark = 0.6f, .spread_taper = 0.3f, .brightness_spread = 0.22f,
	};
	paint_t bark = { .dark = { 0.14f, 0.12f, 0.10f }, .light = { 0.47f, 0.42f, 0.37f }, .tip = { 0.38f, 0.32f, 0.24f }, .tip_from = 0.9f,
		.rough = 0.95f };
	paint_t bud = { .dark = { 0.22f, 0.15f, 0.10f }, .light = { 0.46f, 0.36f, 0.26f }, .tip = { 0.52f, 0.46f, 0.38f }, .tip_from = 0.5f };
	shoot_t shoots[16];
	int count = 0;
	int branches = 4 + (random_float(random) < 0.5f ? 1 : 0);
	float x;
	float y;
	float heading;

	// The stem out of the cut, going on as the leader (its needles only at
	// its top)
	shoots[count++] = (shoot_t){ .fx = 0.5f, .fy = canvas->tall - 0.02f, .angle = random_range(random, -0.08f, 0.08f),
		.bend = random_range(random, -0.15f, 0.15f), .length = 0.85f, .radius = 0.03f, .scale = 1.0f, .taper = 0.3f, .needles_from = 0.72f,
		.crook = 0.05f, .phase = random_range(random, 0.0f, 6.0f) };
	// Its branches, both ways from low on it, reaching far out, turning up
	for (int i = 0; i < branches && count < 14; i++)
	{
		float along = 0.24f + 0.44f * ((float)i + random_range(random, 0.0f, 0.7f)) / (float)branches;
		float side = (i % 2 == 0) == (random_float(random) < 0.5f) ? 1.0f : -1.0f;
		float out = random_range(random, 0.9f, 1.4f) * (1.0f - 0.45f * along);
		float length = random_range(random, 0.5f, 0.64f) * (1.0f - 0.4f * along);
		int parent;

		shoot_point(&shoots[0], along, &x, &y, &heading);
		parent = count;
		shoots[count++] = (shoot_t){ .fx = x, .fy = y, .angle = heading + side * out, .bend = -side * random_range(random, 0.3f, 0.6f),
			.length = length, .radius = 0.016f * (1.0f - 0.4f * along), .scale = 0.95f, .taper = 0.4f, .needles_from = 0.58f,
			.crook = 0.12f, .phase = random_range(random, 0.0f, 6.0f) };
		// (a fork of it, once in a while: a shorter shoot out of its middle)
		if (random_float(random) < 0.65f && count < 15)
		{
			float fork_side = random_float(random) < 0.5f ? 1.0f : -1.0f;
			float fx;
			float fy;
			float fork_heading;

			shoot_point(&shoots[parent], random_range(random, 0.35f, 0.55f), &fx, &fy, &fork_heading);
			shoots[count++] = (shoot_t){ .fx = fx, .fy = fy, .angle = fork_heading + fork_side * random_range(random, 0.45f, 0.8f),
				.bend = -fork_side * random_range(random, 0.2f, 0.4f), .length = length * random_range(random, 0.5f, 0.65f),
				.radius = shoots[parent].radius * 0.6f, .scale = 0.9f, .taper = 0.45f, .needles_from = 0.4f,
				.crook = 0.1f, .phase = random_range(random, 0.0f, 6.0f) };
		}
	}
	draw_twig(canvas, random, &style, &bark, &bud, shoots, count);
}

static void draw_juniper(canvas_t *canvas, random_t *random)
{
	needle_style_t style = {
		.length_low = 0.085f, .length_high = 0.12f, .radius = 0.0075f,
		.angle_low = 0.7f, .angle_high = 1.2f, .curl = 0.08f,
		.per_bundle = 3, .bundles_per_unit = 26.0f, .crowding = 1.25f, .bare = 0.05f, .brush = 0.4f,
		.greens = {
			{ .dark = { 0.07f, 0.14f, 0.11f }, .light = { 0.26f, 0.40f, 0.31f }, .stripe = { 0.62f, 0.72f, 0.70f }, .stripe_width = 0.35f,
				.tip = { 0.36f, 0.46f, 0.32f }, .tip_from = 0.8f },
			{ .dark = { 0.08f, 0.16f, 0.10f }, .light = { 0.30f, 0.44f, 0.30f }, .stripe = { 0.66f, 0.76f, 0.72f }, .stripe_width = 0.3f,
				.tip = { 0.42f, 0.50f, 0.34f }, .tip_from = 0.8f } },
		.behind_dark = 0.5f,
	};
	paint_t bark = { .dark = { 0.20f, 0.12f, 0.08f }, .light = { 0.56f, 0.38f, 0.26f }, .tip = { 0.40f, 0.46f, 0.22f }, .tip_from = 0.75f };
	shoot_t shoots[12];
	int count = 0;

	// A branchy twig: the main shoot, shoots out of it both ways, a few out
	// of those
	shoots[count++] = (shoot_t){ .fx = 0.5f, .fy = canvas->tall - 0.04f, .angle = random_range(random, -0.06f, 0.06f), .bend = random_range(random, -0.2f, 0.2f),
		.length = 1.6f, .radius = 0.012f, .scale = 1.0f };
	for (int i = 0; i < 7 && count < 12; i++)
	{
		float along = 0.18f + 0.11f * (float)i + random_range(random, -0.03f, 0.03f);
		float side = (i % 2 == 0) ? 1.0f : -1.0f;
		float x;
		float y;
		float heading;
		float length = random_range(random, 0.32f, 0.5f) * (1.0f - 0.55f * along);

		shoot_point(&shoots[0], along, &x, &y, &heading);
		shoots[count++] = (shoot_t){ .fx = x, .fy = y, .angle = heading + side * random_range(random, 0.5f, 0.8f),
			.bend = -side * random_range(random, 0.1f, 0.35f), .length = length, .radius = 0.007f, .scale = 0.95f };
	}
	draw_twig(canvas, random, &style, &bark, NULL, shoots, count);
}

// --- Out --------------------------------------------------------------------------------

static void write_to_file(void *context, void *data, int size)
{
	fwrite(data, 1, (size_t)size, (FILE *)context);
}

// Averaged down, the color spread under the clear background, written
static bool finish(const canvas_t *canvas, int width, int height, const char *path)
{
	uint8_t *out = malloc((size_t)width * (size_t)height * 4);
	float *small = malloc(sizeof(float) * (size_t)width * (size_t)height * 4);
	float *spread = malloc(sizeof(float) * (size_t)width * (size_t)height * 4);
	FILE *file;
	bool ok;

	if (out == NULL || small == NULL || spread == NULL)
	{
		free(out);
		free(small);
		free(spread);
		return false;
	}
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			for (int j = 0; j < SUPERSAMPLE; j++)
			{
				for (int i = 0; i < SUPERSAMPLE; i++)
				{
					const float *pixel = &canvas->pixels[((size_t)(y * SUPERSAMPLE + j) * (size_t)canvas->width + (size_t)(x * SUPERSAMPLE + i)) * 4];

					// (the drawing keeps color over coverage, not premultiplied: weight it)
					for (int k = 0; k < 3; k++)
					{
						sum[k] += pixel[k] * pixel[3];
					}
					sum[3] += pixel[3];
				}
			}
			for (int k = 0; k < 4; k++)
			{
				small[((size_t)y * (size_t)width + (size_t)x) * 4 + (size_t)k] = sum[k] / (float)(SUPERSAMPLE * SUPERSAMPLE);
			}
		}
	}
	// The color where it is clear: from the nearest drawn, outwards a
	// pixel a pass (premultiplied: averaged and divided by the weight)
	memcpy(spread, small, sizeof(float) * (size_t)width * (size_t)height * 4);
	for (int pass = 0; pass < 24; pass++)
	{
		bool changed = false;

		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				float *pixel = &spread[((size_t)y * (size_t)width + (size_t)x) * 4];
				float sum[3] = { 0.0f, 0.0f, 0.0f };
				float weight = 0.0f;

				if (pixel[3] > 0.0f)
				{
					continue;
				}
				for (int j = -1; j <= 1; j++)
				{
					for (int i = -1; i <= 1; i++)
					{
						int nx = x + i;
						int ny = y + j;
						const float *neighbor;

						if (nx < 0 || ny < 0 || nx >= width || ny >= height)
						{
							continue;
						}
						neighbor = &small[((size_t)ny * (size_t)width + (size_t)nx) * 4];
						if (neighbor[3] > 0.0f)
						{
							for (int k = 0; k < 3; k++)
							{
								sum[k] += neighbor[k] / neighbor[3];
							}
							weight += 1.0f;
						}
					}
				}
				if (weight > 0.0f)
				{
					// (a tiny weight marks it done; its alpha written clear below)
					for (int k = 0; k < 3; k++)
					{
						pixel[k] = sum[k] / weight * 1e-6f;
					}
					pixel[3] = 1e-6f;
					changed = true;
				}
			}
		}
		memcpy(small, spread, sizeof(float) * (size_t)width * (size_t)height * 4);
		if (!changed)
		{
			break;
		}
	}
	for (size_t i = 0; i < (size_t)width * (size_t)height; i++)
	{
		float alpha = small[i * 4 + 3];

		for (int k = 0; k < 3; k++)
		{
			float color = alpha > 0.0f ? small[i * 4 + (size_t)k] / alpha : 0.0f;

			out[i * 4 + (size_t)k] = (uint8_t)lroundf(clampf(color, 0.0f, 1.0f) * 255.0f);
		}
		out[i * 4 + 3] = (uint8_t)lroundf(clampf(alpha < 1e-5f ? 0.0f : alpha, 0.0f, 1.0f) * 255.0f);
	}
	file = fopen(path, "wb");
	ok = file != NULL && stbi_write_png_to_func(write_to_file, file, width, height, 4, out, width * 4) != 0;
	if (file != NULL)
	{
		fclose(file);
	}
	free(out);
	free(small);
	free(spread);
	return ok;
}

int main(int argument_count, char **arguments)
{
	int width = 640;
	uint32_t seed = 1;
	const char *kind = NULL;
	const char *path = NULL;
	canvas_t canvas;
	random_t random;

	for (int i = 1; i < argument_count; i++)
	{
		if (strcmp(arguments[i], "--width") == 0 && i + 1 < argument_count)
		{
			width = atoi(arguments[++i]);
		}
		else if (strcmp(arguments[i], "--seed") == 0 && i + 1 < argument_count)
		{
			seed = (uint32_t)strtoul(arguments[++i], NULL, 10);
		}
		else if (kind == NULL)
		{
			kind = arguments[i];
		}
		else if (path == NULL)
		{
			path = arguments[i];
		}
	}
	if (kind == NULL || path == NULL || width < 16 || width > 4096)
	{
		fprintf(stderr, "Usage: potop_twig [--width W] [--seed N] pine|juniper OUT.png\n");
		return 1;
	}
	// (as tall as the kind's twig is: the pine's branch tip square,
	// the juniper's twig twice as tall as wide)
	canvas.tall = strcmp(kind, "pine") == 0 ? 1.0f : 2.0f;
	canvas.width = width * SUPERSAMPLE;
	canvas.height = (int)lroundf((float)width * canvas.tall) * SUPERSAMPLE;
	canvas.scale = (float)canvas.width;
	canvas.fit = 1.0f;
	canvas.foot_x = 0.5f;
	canvas.foot_y = canvas.tall;
	canvas.pixels = calloc((size_t)canvas.width * (size_t)canvas.height * 4, sizeof(float));
	if (canvas.pixels == NULL)
	{
		fprintf(stderr, "potop_twig: out of memory\n");
		return 1;
	}
	random.state = (uint64_t)seed * 0x9E3779B97F4A7C15ull + 1u;
	if (strcmp(kind, "pine") == 0)
	{
		draw_pine(&canvas, &random);
	}
	else if (strcmp(kind, "juniper") == 0)
	{
		draw_juniper(&canvas, &random);
	}
	else
	{
		fprintf(stderr, "potop_twig: no kind '%s' (pine, juniper)\n", kind);
		free(canvas.pixels);
		return 1;
	}
	if (!finish(&canvas, width, canvas.height / SUPERSAMPLE, path))
	{
		fprintf(stderr, "potop_twig: could not write %s\n", path);
		free(canvas.pixels);
		return 1;
	}
	free(canvas.pixels);
	printf("potop_twig: %s (%s, seed %u, %d x %d)\n", path, kind, seed, width, canvas.height / SUPERSAMPLE);
	return 0;
}
