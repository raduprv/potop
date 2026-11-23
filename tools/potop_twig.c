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
	float rim;                     // its rim this much darker (a needle's outline against those behind; 0: none)
	float gloss;                   // a bright line along its lit side (0: none)
	float shadow;                  // > 0: a shadow, not paint: what is under it darkened by this much, nothing added where clear
	float foot[3];                 // towards its foot (a needle pale where it leaves its sheath) ...
	float foot_to;                 // ... up to this far along (0: none)
	float bumps;                   // knobs on it (where needles fell: a pine's shoots and stem), 0: none
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
				if (paint->foot_to > 0.0f && along < paint->foot_to)
				{
					color[k] = mixf(paint->foot[k], color[k], along / paint->foot_to);
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
			if (paint->bumps > 0.0f)
			{
				// (a knob: lit on its upper left, shaded on its lower right)
				float here = value_noise((float)x / 11.0f, (float)y / 11.0f);
				float upper = value_noise(((float)x - 3.0f) / 11.0f, ((float)y - 3.0f) / 11.0f);
				float knob = clampf((here - 0.62f) / 0.12f, 0.0f, 1.0f);
				float relief = clampf((upper - here) * 6.0f, -1.0f, 1.0f);

				for (int k = 0; k < 3; k++)
				{
					color[k] *= 1.0f + paint->bumps * knob * (0.45f * -relief - 0.15f);
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
			if (paint->shadow > 0.0f)
			{
				// (soft: thinning to its rim)
				float soft = coverage * (1.0f - fabsf(across));

				for (int k = 0; k < 3; k++)
				{
					pixel[k] *= 1.0f - paint->shadow * soft;
				}
				continue;
			}
			if (paint->rim > 0.0f)
			{
				float edge = clampf((fabsf(across) - 0.55f) / 0.45f, 0.0f, 1.0f);

				for (int k = 0; k < 3; k++)
				{
					color[k] *= 1.0f - paint->rim * edge * edge;
				}
			}
			if (paint->gloss > 0.0f)
			{
				// (on the lit side, a little off the middle)
				float lit = across * (light_side >= 0.0f ? 1.0f : -1.0f);
				float line = clampf(1.0f - fabsf(lit - 0.35f) / 0.22f, 0.0f, 1.0f);

				for (int k = 0; k < 3; k++)
				{
					color[k] = mixf(color[k], fminf(color[k] * 1.6f + 0.06f, 1.0f), paint->gloss * line);
				}
			}
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

// --- A needle --------------------------------------------------------------------------

// What a conifer's needles look like: their outline, how round they are,
// their colors; one for every needle of a kind (each needle varies it a
// little). A pine's: long, round, rounded tips, a pale foot; a spruce's:
// short, four sided, sharp; a fir's: flat, notched, two pale stripes under
typedef struct
{
	float foot_narrow;             // the foot this much narrower than the middle (where it leaves the twig)
	float tip_round;               // 1: the tip a half round; 0: drawn out to a point
	float tip_length;              // the tip's share of the length (where it narrows)
	float flat;                    // 0: round across (lit like a cylinder); 1: flat (lit evenly)
	float foot[3];                 // the color at the foot ...
	float middle[3];               // ... along most of it ...
	float tip[3];                  // ... and at the tip
	float foot_to;                 // the foot's color up to this far along
	float tip_from;                // the tip's color from this far along
	float stripes;                 // pale stripes along it (stomata), 0: none, 1: two plain ones
	float stripe_color[3];
	float streaks;                 // fine lines along it, lighter and darker (0: monochrome)
	float gloss;                   // a soft bright band along its lit side
	float shade;                   // how dark its shadowed side gets (0..1)
	float cone_ratio;              // > 0: drawn to be wrapped on a cone whose foot ring is this share of its rim's: wider towards its foot by as
	                               // much as the cone narrows there (as wide as half_width at its tip, at most cone_widest times that)
	float cone_widest;
} needle_shape_t;

// One needle: from (ax, ay) along a curve through (cx, cy) to (bx, by),
// half_width at its widest, drawn in the shape's outline and colors;
// brightness and hue_shift (towards yellow) its own; dry: a brown tip from
// this far along (1: none)
static void draw_needle_shape(canvas_t *canvas, const needle_shape_t *shape, float ax, float ay, float cx, float cy, float bx, float by,
	float half_width, float brightness, float hue_shift, float dry, uint32_t grain_seed)
{
	enum { PIECES = 12 };
	float points[PIECES + 1][2];
	float lengths[PIECES + 1];
	float s = canvas->scale * canvas->fit;
	float total;
	float left = 1e9f;
	float right = -1e9f;
	float top = 1e9f;
	float bottom = -1e9f;
	float grain_offset = (float)(grain_seed % 1000u) * 7.31f;

	// The curve in the drawing's pixels, and how far along it each point is
	for (int i = 0; i <= PIECES; i++)
	{
		float t = (float)i / (float)PIECES;
		float u = 1.0f - t;
		float x = u * u * ax + 2.0f * u * t * cx + t * t * bx;
		float y = u * u * ay + 2.0f * u * t * cy + t * t * by;

		points[i][0] = (canvas->foot_x + (x - canvas->foot_x) * canvas->fit) * canvas->scale;
		points[i][1] = (canvas->foot_y + (y - canvas->foot_y) * canvas->fit) * canvas->scale;
		lengths[i] = i == 0 ? 0.0f
			: lengths[i - 1] + sqrtf((points[i][0] - points[i - 1][0]) * (points[i][0] - points[i - 1][0])
				+ (points[i][1] - points[i - 1][1]) * (points[i][1] - points[i - 1][1]));
		left = fminf(left, points[i][0]);
		right = fmaxf(right, points[i][0]);
		top = fminf(top, points[i][1]);
		bottom = fmaxf(bottom, points[i][1]);
	}
	total = fmaxf(lengths[PIECES], 1e-3f);
	half_width *= s;
	{
		int x0 = (int)floorf(left - half_width - 2.0f);
		int x1 = (int)ceilf(right + half_width + 2.0f);
		int y0 = (int)floorf(top - half_width - 2.0f);
		int y1 = (int)ceilf(bottom + half_width + 2.0f);

		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 >= canvas->width ? canvas->width - 1 : x1;
		y1 = y1 >= canvas->height ? canvas->height - 1 : y1;
		for (int y = y0; y <= y1; y++)
		{
			for (int x = x0; x <= x1; x++)
			{
				float px = (float)x + 0.5f;
				float py = (float)y + 0.5f;
				float best = 1e9f;
				float along = 0.0f;
				float side = 0.0f;
				float width;
				float across;
				float coverage;
				float lit;
				float color[3];
				float *pixel;

				// (the nearest point of the curve: how far along, which side)
				for (int i = 0; i < PIECES; i++)
				{
					float dx = points[i + 1][0] - points[i][0];
					float dy = points[i + 1][1] - points[i][1];
					float length_squared = dx * dx + dy * dy;
					float t = length_squared > 0.0f ? clampf(((px - points[i][0]) * dx + (py - points[i][1]) * dy) / length_squared, 0.0f, 1.0f) : 0.0f;
					float ex = px - (points[i][0] + t * dx);
					float ey = py - (points[i][1] + t * dy);
					float distance = ex * ex + ey * ey;

					if (distance < best)
					{
						best = distance;
						along = (lengths[i] + t * sqrtf(length_squared)) / total;
						// (signed: its right of the heading positive)
						side = (dx * ey - dy * ex) >= 0.0f ? 1.0f : -1.0f;
					}
				}
				best = sqrtf(best);
				// The outline: a narrower foot, full along, the tip a half
				// round or drawn out
				width = half_width * (1.0f - shape->foot_narrow * powf(fmaxf(1.0f - along / 0.12f, 0.0f), 2.0f));
				if (shape->cone_ratio > 0.0f)
				{
					width *= fminf(1.0f / (shape->cone_ratio + (1.0f - shape->cone_ratio) * along), shape->cone_widest);
				}
				if (along > 1.0f - shape->tip_length)
				{
					float into = (along - (1.0f - shape->tip_length)) / fmaxf(shape->tip_length, 1e-3f);
					float round = sqrtf(fmaxf(1.0f - into * into, 0.0f));
					float pointed = 1.0f - into;

					width *= mixf(pointed, round, shape->tip_round);
				}
				coverage = clampf(width - best + 0.5f, 0.0f, 1.0f);
				if (coverage <= 0.0f || width <= 0.0f)
				{
					continue;
				}
				across = clampf(side * best / width, -1.0f, 1.0f);
				// Lit from the upper left: a cylinder (or a flat strip)
				{
					float normal_x = across * (1.0f - shape->flat);
					float normal_z = sqrtf(fmaxf(1.0f - normal_x * normal_x, 0.0f));
					// (which side of the heading faces the light: the heading's
					// right is towards the light when it points up and left)
					float dx = points[PIECES][0] - points[0][0];
					float dy = points[PIECES][1] - points[0][1];
					float facing = (-dy * -0.7f + dx * -0.7f) >= 0.0f ? 1.0f : -1.0f;

					lit = clampf(0.55f + 0.45f * (normal_x * facing * 0.8f + normal_z * 0.6f), 0.0f, 1.0f);
					lit = mixf(1.0f - shape->shade, 1.0f, lit);
					for (int k = 0; k < 3; k++)
					{
						float base = shape->middle[k];

						if (along < shape->foot_to)
						{
							base = mixf(shape->foot[k], base, along / fmaxf(shape->foot_to, 1e-3f));
						}
						if (along > shape->tip_from)
						{
							base = mixf(base, shape->tip[k], (along - shape->tip_from) / fmaxf(1.0f - shape->tip_from, 1e-3f));
						}
						// (towards yellow, its own)
						base += hue_shift * (k == 0 ? 0.06f : (k == 1 ? 0.03f : -0.03f));
						if (along > dry)
						{
							float brown[3] = { 0.44f, 0.32f, 0.19f };

							base = mixf(base, brown[k], clampf((along - dry) / 0.06f, 0.0f, 1.0f));
						}
						color[k] = base * lit * brightness;
					}
					// Pale stripes along it
					if (shape->stripes > 0.0f)
					{
						float stripe = clampf(1.0f - fabsf(fabsf(across) - 0.45f) / 0.18f, 0.0f, 1.0f) * shape->stripes;

						for (int k = 0; k < 3; k++)
						{
							color[k] = mixf(color[k], shape->stripe_color[k] * lit * brightness, stripe * 0.7f);
						}
					}
					// Fine lines along it, lighter and darker
					if (shape->streaks > 0.0f)
					{
						float streak = value_noise(across * 6.0f + grain_offset, along * total / 40.0f) - 0.5f;
						float fleck = value_noise(across * 3.0f - grain_offset, along * total / 9.0f) - 0.5f;

						for (int k = 0; k < 3; k++)
						{
							color[k] *= 1.0f + shape->streaks * (streak * 0.5f + fleck * 0.25f);
						}
					}
					// A soft bright band on the lit side
					if (shape->gloss > 0.0f)
					{
						float band = clampf(1.0f - fabsf(across * facing - 0.3f) / 0.3f, 0.0f, 1.0f);

						band = band * band * (3.0f - 2.0f * band);
						for (int k = 0; k < 3; k++)
						{
							color[k] += shape->gloss * band * 0.12f * brightness;
						}
					}
				}
				pixel = &canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4];
				for (int k = 0; k < 3; k++)
				{
					pixel[k] = clampf(color[k], 0.0f, 1.0f) * coverage + pixel[k] * (1.0f - coverage);
				}
				pixel[3] = coverage + pixel[3] * (1.0f - coverage);
			}
		}
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
	const paint_t *bark;           // its own bark (NULL: the twig's)
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

		angle = shoot->angle + shoot->bend * u + shoot->crook * (sinf(u * 9.0f + shoot->phase) * 0.5f + sinf(u * 23.0f + shoot->phase * 1.7f) * 0.3f
			+ sinf(u * 47.0f + shoot->phase * 2.3f) * 0.2f);
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
	float dry;                     // its tip dried brown, from this far along (1: green to its tip)
	float gloss;                   // how glossy, of the style's
	bool sheath;                   // the first of its pair: its sheath drawn at its foot
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
	bool pairs_together;           // a bundle's needles side by side, a little apart (a pine's pairs), not round the shoot
	float rim;                     // each needle's outline darker by this much (they stand apart)
	float gloss;                   // a bright line along each
	float shadow;                  // each casts a shadow this dark on those behind it (0: none)
	float tip_radius;              // a needle's half width at its tip, of its foot's (0: 0.15, sharp)
	float unevenness;              // how much needles differ (thickness, length, curve), 0..1
	float dry_share;               // the needles with dried tips
	float broken_share;            // the needles broken short (a dry stub of a tip)
	paint_t sheath;                // a pair's sheath at its foot (its tip_from 0: none)
	float foot_pale[3];            // a needle's color where it leaves its sheath
	const needle_shape_t *shape;   // the needles drawn in this outline and colors (NULL: as shaded strokes)
	float order;                   // 0: bundles round the shoot at random; 1: evenly in a spiral (the golden angle), their angles and lengths alike
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
	int bundle_index = 0;

	while (t < 1.0f && count < NEEDLES_MAX - 16)
	{
		float density = style->bundles_per_unit * mixf(1.0f, style->crowding, t * t) / fmaxf(scale, 0.2f);
		float x;
		float y;
		float heading;

		float bundle_around = 0.0f;

		shoot_point(shoot, t, &x, &y, &heading);
		for (int k = 0; k < style->per_bundle; k++)
		{
			needle_t *needle = &needles[count++];
			// (a bundle's needles round the shoot: seen from the side,
			// some to the left, some to the right, some towards us)
			// (a pine's pair: from one sheath, a little apart)
			// (in order: the bundles round the shoot by the golden angle, as
			// they grow, a little jitter)
			float first = mixf(random_range(random, 0.0f, 2.0f * PI_F), (float)bundle_index * 2.39996f + random_range(random, -0.15f, 0.15f), style->order);
			float around = style->pairs_together ? (k == 0 ? first : bundle_around + mixf(random_range(random, 0.25f, 0.55f), 0.4f, style->order))
				: first + (float)k * 2.0f * PI_F / (float)style->per_bundle;

			if (k == 0)
			{
				bundle_around = around;
			}
			float side = sinf(around);
			float tuft = clampf((t - style->bare) / fmaxf(1.0f - style->bare, 0.01f), 0.0f, 1.0f);
			float spread = mixf(random_range(random, style->angle_low, style->angle_high), 0.5f * (style->angle_low + style->angle_high)
				+ random_range(random, -0.05f, 0.05f), style->order) * (0.55f + 0.45f * fabsf(side))
				* mixf(1.0f, style->spread_taper > 0.0f ? style->spread_taper : 1.0f, tuft);

			needle->x = x + random_range(random, -0.004f, 0.004f);
			needle->y = y;
			needle->angle = heading + (side >= 0.0f ? spread : -spread);
			// (one pointing at us or away is seen shorter)
			needle->length = mixf(random_range(random, style->length_low, style->length_high), 0.5f * (style->length_low + style->length_high)
				* random_range(random, 0.94f, 1.06f), style->order) * scale * mixf(0.8f, 1.05f, t)
				* (1.0f - 0.55f * fabsf(cosf(around)) * sinf(fminf(spread / (0.55f + 0.45f * fabsf(side)), 1.5f)));
			needle->curl = (side >= 0.0f ? -1.0f : 1.0f) * style->curl * mixf(random_range(random, 0.5f, 1.5f), random_range(random, 0.85f, 1.15f), style->order)
				+ random_range(random, -0.5f, 0.5f) * style->unevenness;
			needle->radius = style->radius * scale * random_range(random, 0.85f, 1.15f) * (1.0f + random_range(random, -0.35f, 0.25f) * style->unevenness);
			needle->length *= 1.0f + random_range(random, -0.3f, 0.15f) * style->unevenness;
			needle->dry = random_float(random) < style->dry_share ? random_range(random, 0.72f, 0.92f) : 1.0f;
			if (random_float(random) < style->broken_share)
			{
				needle->length *= random_range(random, 0.45f, 0.7f);
				needle->dry = 0.8f;
			}
			needle->gloss = random_range(random, 0.2f, 1.2f);
			needle->sheath = k == 0;
			needle->depth = cosf(around);
			needle->hue = random_float(random);
			needle->brightness = 1.0f + random_range(random, -style->brightness_spread, style->brightness_spread);
		}
		t += 1.0f / fmaxf(density * length, 1.0f);
		bundle_index++;
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
		needle->radius = style->radius * scale * (1.0f + random_range(random, -0.3f, 0.2f) * style->unevenness);
		needle->dry = 1.0f;
		needle->gloss = random_range(random, 0.2f, 1.2f);
		needle->sheath = false;
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
		// (a curve leaving its foot heading at its angle, reaching its tip
		// heading at its angle and curl: the control on the first heading)
		control_x = needle->x + sinf(needle->angle) * needle->length * 0.5f;
		control_y = needle->y - cosf(needle->angle) * needle->length * 0.5f;
		end_x = control_x + sinf(end_angle) * needle->length * 0.5f;
		end_y = control_y - cosf(end_angle) * needle->length * 0.5f;
		(void)middle_angle;
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
		paint.rim = style->rim;
		paint.gloss = style->gloss * needle->gloss;
		if (needle->dry < 1.0f)
		{
			// (a dried tip: brown, from where it dried)
			paint.tip[0] = 0.42f * darken;
			paint.tip[1] = 0.31f * darken;
			paint.tip[2] = 0.18f * darken;
			paint.tip_from = needle->dry;
		}
		if (style->foot_pale[1] > 0.0f)
		{
			for (int k = 0; k < 3; k++)
			{
				paint.foot[k] = style->foot_pale[k] * darken;
			}
			paint.foot_to = 0.12f;
		}
		if (style->shadow > 0.0f)
		{
			// (its shadow on what is behind it: down and to the right of
			// it, away from the light, wide and soft)
			paint_t shade = { .shadow = style->shadow, .tip_from = 1.0f };
			float offset = needle->radius * 2.4f;

			draw_curve(canvas, needle->x + offset, needle->y + offset, control_x + offset, control_y + offset, end_x + offset, end_y + offset,
				needle->radius * 3.2f, needle->radius * 1.2f, &shade);
		}
		if (style->shape != NULL)
		{
			draw_needle_shape(canvas, style->shape, needle->x, needle->y, control_x, control_y, end_x, end_y, needle->radius, darken,
				(needle->hue - 0.5f) * 1.2f, needle->dry, (uint32_t)i * 2654435761u);
		}
		else
		{
			draw_curve(canvas, needle->x, needle->y, control_x, control_y, end_x, end_y, needle->radius,
				needle->radius * (style->tip_radius > 0.0f ? style->tip_radius : 0.15f), &paint);
		}
		if (needle->sheath && style->sheath.tip_from > 0.0f)
		{
			// (the pair's sheath: a short brown grey sleeve at their foot)
			draw_segment(canvas, needle->x, needle->y, needle->radius * 1.05f, needle->x + sinf(needle->angle) * needle->radius * 2.2f,
				needle->y - cosf(needle->angle) * needle->radius * 2.2f, needle->radius * 0.8f, 0.0f, 1.0f, &style->sheath);
		}
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

			draw_segment(canvas, last_x, last_y, shoot->radius * mixf(1.0f, taper, t0), x, y, shoot->radius * mixf(1.0f, taper, t), t0, t,
				shoot->bark != NULL ? shoot->bark : bark);
			last_x = x;
			last_y = y;
		}
		// Its knobs (where needles fell), if its bark has them: on its rim
		// standing out of its outline, on its face as little rounds; each
		// darker towards its foot, a shadow under it
		{
			const paint_t *own = shoot->bark != NULL ? shoot->bark : bark;
			paint_t knob = *own;
			paint_t shade = { .shadow = 0.35f, .tip_from = 1.0f };
			float step = shoot->radius * 0.3f;

			knob.bumps = 0.0f;
			knob.rough = 0.2f;
			for (int k = 0; k < 3; k++)
			{
				knob.tip[k] = own->light[k] * 1.05f;
				knob.dark[k] = own->dark[k] * 0.55f;
				knob.light[k] = own->light[k] * 0.62f;
			}
			knob.tip_from = 0.0f;
			int knob_index = 0;

			for (float t = 0.04f; own->bumps > 0.0f && t < 0.96f; t += step / shoot->length * mixf(0.5f, 1.5f, hash_unit(i * 13, knob_index)))
			{
				float x;
				float y;
				float knob_heading;
				float radius = shoot->radius * mixf(1.0f, shoot->taper > 0.0f ? shoot->taper : 0.45f, t);
				// (in a spiral round it, as the needles were: by the golden
				// angle; those round its back unseen)
				float around = (float)knob_index * 2.39996f + (hash_unit(knob_index, i * 5) - 0.5f) * 1.3f;
				float where = sinf(around);

				knob_index++;
				if (cosf(around) < -0.25f)
				{
					continue;
				}
				float side = where >= 0.0f ? 1.0f : -1.0f;
				float out = side * 1.0f;
				float size = radius * mixf(0.18f, 0.3f, hash_unit(i, (int)(t * 613.0f)));
				float foot_x;
				float foot_y;
				float tip_x;
				float tip_y;


				shoot_point(shoot, t, &x, &y, &knob_heading);
				// (across its face: anywhere from one rim to the other; on the
				// rim it points out, on the face along the shoot, seen end on)
				foot_x = x + cosf(knob_heading) * where * radius * 0.85f;
				foot_y = y + sinf(knob_heading) * where * radius * 0.85f;
				if (fabsf(where) > 0.75f)
				{
					tip_x = foot_x + sinf(knob_heading + out) * size * 1.4f;
					tip_y = foot_y - cosf(knob_heading + out) * size * 1.4f;
				}
				else
				{
					tip_x = foot_x + sinf(knob_heading) * size * 0.9f;
					tip_y = foot_y - cosf(knob_heading) * size * 0.9f;
				}
				draw_segment(canvas, foot_x + size * 0.6f, foot_y + size * 0.6f, size * 1.3f, tip_x + size * 0.6f, tip_y + size * 0.6f, size,
					0.0f, 1.0f, &shade);
				// (lumpy: two or three pieces of it, each a little off and
				// its own size)
				for (int piece = 0; piece < 3; piece++)
				{
					float jitter_x = (hash_unit(i * 31 + piece, (int)(t * 1777.0f)) - 0.5f) * size * 1.1f;
					float jitter_y = (hash_unit(i * 17 + piece, (int)(t * 2333.0f)) - 0.5f) * size * 1.1f;
					float piece_size = size * mixf(0.5f, 1.0f, hash_unit(piece * 7 + i, (int)(t * 911.0f)));

					if (piece == 2 && hash_unit(i, (int)(t * 1291.0f)) < 0.5f)
					{
						break;
					}
					draw_segment(canvas, foot_x + jitter_x, foot_y + jitter_y, piece_size, tip_x + jitter_x * 0.6f, tip_y + jitter_y * 0.6f,
						piece_size * mixf(0.4f, 0.8f, hash_unit(piece, (int)(t * 733.0f) + i)), 0.0f, 1.0f, &knob);
				}
			}
			// Small cracks in it: thin dark lines along it, here and there
			for (int crack = 0; own->bumps > 0.0f && crack < (int)(shoot->length / shoot->radius * 1.2f); crack++)
			{
				paint_t line = { .shadow = 0.55f, .tip_from = 1.0f };
				float t = hash_unit(i * 41 + 7, crack);
				float where = (hash_unit(crack, i * 3 + 11) * 2.0f - 1.0f) * 0.8f;
				float long_ = shoot->radius * mixf(1.5f, 4.0f, hash_unit(crack * 3, i));
				float x;
				float y;
				float heading;
				float radius;
				float tilt = (hash_unit(crack, i * 9) - 0.5f) * 0.5f;

				shoot_point(shoot, t, &x, &y, &heading);
				radius = shoot->radius * mixf(1.0f, shoot->taper > 0.0f ? shoot->taper : 0.45f, t);
				x += cosf(heading) * where * radius;
				y += sinf(heading) * where * radius;
				draw_segment(canvas, x, y, radius * 0.07f, x + sinf(heading + tilt) * long_, y - cosf(heading + tilt) * long_, radius * 0.04f, 0.0f, 1.0f,
					&line);
			}
		}

	}
	draw_needles(canvas, needles, count, style, true);
	// The buds at the shoots' tips, in the brushes' middles over them
	for (int i = 0; i < shoot_count && bud != NULL; i++)
	{
		float x;
		float y;
		float heading;

		if (shoots[i].bare)
		{
			continue;
		}
		// (as big as a shoot's, whatever its stem)
		float size = fminf(shoots[i].radius, 0.012f) * 1.3f;

		shoot_point(&shoots[i], 0.97f, &x, &y, &heading);
		draw_segment(canvas, x, y, size * 1.3f, x + sinf(heading) * size * 5.5f, y - cosf(heading) * size * 5.5f, size * 0.4f, 0.0f, 1.0f, bud);
	}
	free(needles);
}

// A mountain pine's twig: a scaly stem up the middle, shoots out of it both
// ways, every shoot (the stem's top too) clothed nearly all along in stiff
// needles in pairs, pointing forward round it like a bottle brush, a brown
// bud at its tip; the reddish brown shoot showing between the needles
// A mountain pine's needles, bark and buds (its twig's, its brush's, its
// brush seen end on)
typedef struct
{
	needle_style_t style;
	needle_shape_t needle;
	paint_t bark;
	paint_t bud;
} pine_parts_t;

static void pine_parts(pine_parts_t *parts)
{
	needle_style_t style = {
		.length_low = 0.187f, .length_high = 0.253f, .radius = 0.0046f,
		.angle_low = 1.2f, .angle_high = 1.55f, .curl = 0.6f,
		.per_bundle = 2, .bundles_per_unit = 144.0f, .crowding = 1.1f, .bare = 0.08f, .brush = 0.6f,
		.greens = {
			{ .dark = { 0.16f, 0.21f, 0.12f }, .light = { 0.40f, 0.47f, 0.29f }, .tip = { 0.40f, 0.46f, 0.28f }, .tip_from = 1.0f },
			{ .dark = { 0.20f, 0.24f, 0.13f }, .light = { 0.48f, 0.52f, 0.31f }, .tip = { 0.47f, 0.51f, 0.30f }, .tip_from = 1.0f } },
		.behind_dark = 0.25f, .spread_taper = 0.65f, .brightness_spread = 0.18f,
		.pairs_together = true, .rim = 0.12f, .gloss = 0.14f, .shadow = 0.14f, .tip_radius = 0.35f,
		.unevenness = 0.15f, .dry_share = 0.03f, .broken_share = 0.02f, .order = 0.97f,
		.sheath = { .dark = { 0.18f, 0.11f, 0.07f }, .light = { 0.42f, 0.28f, 0.18f }, .tip = { 0.40f, 0.30f, 0.20f }, .tip_from = 0.5f },
		.foot_pale = { 0.46f, 0.50f, 0.31f },
	};
	// The needle: round, a little narrower at its foot, its tip rounded;
	// pale yellowish at its foot, grey green along, a touch lighter at the
	// tip, fine streaks along it, a soft gloss
	needle_shape_t needle = {
		.foot_narrow = 0.3f, .tip_round = 0.75f, .tip_length = 0.07f, .flat = 0.15f,
		.foot = { 0.50f, 0.48f, 0.28f }, .middle = { 0.36f, 0.40f, 0.22f }, .tip = { 0.40f, 0.43f, 0.24f },
		.foot_to = 0.12f, .tip_from = 0.82f,
		.stripes = 0.12f, .stripe_color = { 0.44f, 0.48f, 0.34f },
		.streaks = 0.35f, .gloss = 0.7f, .shade = 0.45f,
	};

	// (the stem and its shoots one bark: grey brown, knobbly, reddish
	// towards their tips)
	paint_t bark = { .dark = { 0.17f, 0.13f, 0.10f }, .light = { 0.48f, 0.40f, 0.32f }, .tip = { 0.38f, 0.30f, 0.22f }, .tip_from = 0.55f,
		.rough = 0.45f, .bumps = 1.0f };
	paint_t bud = { .dark = { 0.16f, 0.09f, 0.05f }, .light = { 0.46f, 0.30f, 0.19f }, .tip = { 0.50f, 0.40f, 0.30f }, .tip_from = 0.7f,
		.rough = 0.9f };
	parts->style = style;
	parts->needle = needle;
	parts->bark = bark;
	parts->bud = bud;
	parts->style.shape = &parts->needle;
}

static void draw_pine(canvas_t *canvas, random_t *random)
{
	static pine_parts_t parts;
	needle_style_t style;
	paint_t stem_bark;
	paint_t bud;

	pine_parts(&parts);
	style = parts.style;
	stem_bark = parts.bark;
	bud = parts.bud;
	shoot_t shoots[10];
	int count = 0;
	int sides = 5;
	float x;
	float y;
	float heading;
	float first_side = random_float(random) < 0.5f ? 1.0f : -1.0f;

	// The stem, its needles from a little under half way up
	shoots[count++] = (shoot_t){ .fx = 0.5f, .fy = canvas->tall - 0.02f, .angle = random_range(random, 0.08f, 0.18f) * first_side,
		.bend = -first_side * random_range(random, 0.15f, 0.3f), .length = canvas->tall * 0.86f, .radius = 0.032f, .scale = 1.0f, .taper = 0.4f,
		.needles_from = 0.55f, .crook = 0.14f, .phase = random_range(random, 0.0f, 6.0f) };
	// The shoots, alternately, the lower longer, up and out
	for (int i = 0; i < sides && count < 10; i++)
	{
		float along = 0.14f + 0.5f * ((float)i + random_range(random, 0.0f, 0.3f)) / (float)sides;
		float side = (i % 2 == 0) ? first_side : -first_side;
		float length = random_range(random, 0.42f, 0.5f) * (1.0f - 0.45f * along) * canvas->tall * 0.66f;

		shoot_point(&shoots[0], along, &x, &y, &heading);
		shoots[count++] = (shoot_t){ .fx = x, .fy = y, .angle = heading + side * random_range(random, 0.75f, 0.95f),
			.bend = -side * random_range(random, 0.1f, 0.25f), .length = length, .radius = 0.011f, .scale = 0.95f, .taper = 0.55f,
			.needles_from = 0.28f, .crook = 0.18f, .phase = random_range(random, 0.0f, 6.0f) };
	}
	draw_twig(canvas, random, &style, &stem_bark, &bud, shoots, count);
}

// A mountain pine's brush: one shoot, bare and knobbly low down, clothed
// in its needles above, its bud at the tip: the plant grower stands three
// of them round each shoot (a star) and a rosette (draw_pine_rosette) at
// its end, so it is round from every side
static void draw_pine_brush(canvas_t *canvas, random_t *random)
{
	static pine_parts_t parts;
	needle_style_t style;
	shoot_t shoot;

	pine_parts(&parts);
	style = parts.style;
	// (one shoot the picture's length: its needles long beside it)
	style.length_low = 0.36f;
	style.length_high = 0.46f;
	style.radius = 0.0072f;
	style.bundles_per_unit = 80.0f;
	style.brush = 1.6f;
	shoot = (shoot_t){ .fx = 0.5f, .fy = canvas->tall - 0.02f, .angle = random_range(random, -0.04f, 0.04f),
		.bend = random_range(random, -0.12f, 0.12f), .length = canvas->tall * 0.86f, .radius = 0.028f, .scale = 1.0f, .taper = 0.45f,
		.needles_from = 0.36f, .crook = 0.1f, .phase = random_range(random, 0.0f, 6.0f) };
	draw_twig(canvas, random, &style, &parts.bark, &parts.bud, &shoot, 1);
}

// The same brush seen end on, from its tip: its needles radiating round the
// bud, those nearer the tip in front and shorter (they point more forward),
// those further back behind and darker
static void draw_pine_rosette(canvas_t *canvas, random_t *random)
{
	static pine_parts_t parts;
	needle_t *needles = malloc(sizeof(needle_t) * NEEDLES_MAX);
	int count = 0;
	int bundles = 140;
	float center = 0.5f;

	if (needles == NULL)
	{
		return;
	}
	pine_parts(&parts);
	parts.style.radius = 0.0085f;
	parts.style.behind_dark = 0.45f;
	canvas->fit = 1.0f;
	canvas->foot_x = 0.5f;
	canvas->foot_y = 0.5f;
	for (int b = 0; b < bundles; b++)
	{
		// (along the shoot: 0 its needles' foot, 1 its tip)
		float along = (float)b / (float)bundles;
		float around = (float)b * 2.39996f + random_range(random, -0.15f, 0.15f);

		for (int k = 0; k < 2 && count < NEEDLES_MAX; k++)
		{
			needle_t *needle = &needles[count++];
			float heading_around = around + (k == 0 ? 0.0f : 0.4f);
			// (how far out it points from the shoot: less near the tip)
			float spread = random_range(random, 1.2f, 1.5f) * mixf(1.0f, 0.55f, along * along);
			float length = random_range(random, 0.36f, 0.47f) * sinf(spread);
			float start = random_range(random, 0.02f, 0.07f) * (1.0f - along * 0.5f);

			memset(needle, 0, sizeof(*needle));
			needle->x = center + sinf(heading_around) * start;
			needle->y = center - cosf(heading_around) * start;
			needle->angle = heading_around + random_range(random, -0.08f, 0.08f);
			needle->length = length;
			needle->curl = random_range(random, -0.25f, 0.25f);
			needle->radius = parts.style.radius * random_range(random, 0.9f, 1.1f);
			needle->depth = along * 2.0f - 1.0f;
			needle->hue = random_float(random);
			needle->brightness = 1.0f + random_range(random, -0.15f, 0.15f);
			needle->dry = random_float(random) < 0.03f ? 0.85f : 1.0f;
			needle->gloss = random_range(random, 0.4f, 1.0f);
			needle->sheath = false;
		}
	}
	qsort(needles, (size_t)count, sizeof(needle_t), compare_needles);
	draw_needles(canvas, needles, count, &parts.style, false);
	draw_needles(canvas, needles, count, &parts.style, true);
	// The bud in the middle, seen end on: a round of scales
	draw_segment(canvas, center, center, 0.045f, center + 0.004f, center - 0.004f, 0.04f, 0.0f, 1.0f, &parts.bud);
	free(needles);
}

// A few of a mountain pine's needles side by side, their feet together at
// the bottom, fanning a little: the plant grower stands strips of it round
// a shoot as open cones (kind brushes)
static void draw_pine_needles(canvas_t *canvas, random_t *random)
{
	static pine_parts_t parts;
	const int count = 4;

	pine_parts(&parts);
	canvas->fit = 1.0f;
	canvas->foot_x = 0.5f;
	canvas->foot_y = canvas->tall;
	for (int k = 0; k < count; k++)
	{
		float foot_x = 0.5f + ((float)k - 1.5f) * 0.075f + random_range(random, -0.02f, 0.02f);
		float lean = ((float)k - 1.5f) * 0.03f + random_range(random, -0.02f, 0.02f);
		float curl = random_range(random, -0.06f, 0.06f);
		float length = canvas->tall * random_range(random, 0.86f, 0.98f);
		float foot_y = canvas->tall - 0.02f;
		float control_x = foot_x + sinf(lean) * length * 0.5f;
		float control_y = foot_y - cosf(lean) * length * 0.5f;
		float end_x = control_x + sinf(lean + curl) * length * 0.5f;
		float end_y = control_y - cosf(lean + curl) * length * 0.5f;
		float dry = random_float(random) < 0.05f ? random_range(random, 0.75f, 0.9f) : 1.0f;

		draw_needle_shape(canvas, &parts.needle, foot_x, foot_y, control_x, control_y, end_x, end_y, 0.06f,
			1.0f + random_range(random, -0.15f, 0.15f), random_range(random, -0.5f, 0.5f), dry, random_next(random));
	}
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

// --- Ferns ------------------------------------------------------------------------------

// A leaf's color and how it is marked
typedef struct
{
	float base[3];                 // its blade
	float vein[3];                 // along its middle
	float edge[3];                 // towards its rim
	float vein_width;              // of the half width
	float mottle;                  // the fine unevenness (0: none)
	float costa[3];                // a pinna's midrib
} blade_paint_t;

// A flat blade from (bx, by) heading at angle (radians, 0 up the picture,
// positive to the right) length long, at most half_width wide each side,
// bent by curl over it: its outline round at the foot, pointed at the tip
// (sharpness), its rim cut into teeth (teeth of them each side, tooth deep,
// of the half width, the teeth leaning towards the tip). Drawn over what is
// there; the tip's side of a tooth sloped, its foot's side steep
static void draw_blade(canvas_t *canvas, float bx, float by, float angle, float length, float half_width, float curl, int teeth, float tooth,
	float foot_width, const blade_paint_t *paint)
{
	float s = canvas->scale;
	float reach = (length + half_width) * s + 2.0f;
	int left = (int)floorf(bx * s - reach);
	int right = (int)ceilf(bx * s + reach);
	int top = (int)floorf(by * s - reach);
	int bottom = (int)ceilf(by * s + reach);
	float along_x = sinf(angle);
	float along_y = -cosf(angle);

	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right >= canvas->width ? canvas->width - 1 : right;
	bottom = bottom >= canvas->height ? canvas->height - 1 : bottom;
	for (int y = top; y <= bottom; y++)
	{
		for (int x = left; x <= right; x++)
		{
			float px = ((float)x + 0.5f) / s - bx;
			float py = ((float)y + 0.5f) / s - by;
			float u = px * along_x + py * along_y;
			float v = px * along_y * -1.0f * -1.0f;
			float t;
			float bend;
			float width;
			float across;
			float coverage;
			float color[3];
			float *pixel;

			// (across: to the right of the heading; the curl shifts the
			// midline sideways as a parabola along it)
			v = px * -along_y + py * along_x;
			if (u < 0.0f || u > length)
			{
				continue;
			}
			t = u / length;
			bend = curl * length * t * t * 0.5f;
			v -= bend;
			// The outline: a foot this wide, swelling, then narrowing to
			// the tip
			width = half_width * (foot_width + (1.0f - foot_width) * sinf(PI_F * fminf(t * 1.6f, 1.0f) * 0.5f)) * powf(fmaxf(1.0f - t, 0.0f), 0.55f);
			if (teeth > 0 && tooth > 0.0f)
			{
				float phase = t * (float)teeth;
				float within = phase - floorf(phase);

				// (a saw's tooth: rising slowly towards the tip, dropping back)
				width *= 1.0f - tooth * (1.0f - within) * (1.0f - within);
			}
			coverage = clampf((width - fabsf(v)) * s + 0.5f, 0.0f, 1.0f);
			if (coverage <= 0.0f)
			{
				continue;
			}
			across = fabsf(v) / fmaxf(width, 1e-5f);
			for (int k = 0; k < 3; k++)
			{
				color[k] = mixf(paint->base[k], paint->edge[k], clampf((across - 0.6f) / 0.4f, 0.0f, 1.0f));
				if (across < paint->vein_width)
				{
					color[k] = mixf(color[k], paint->vein[k], 1.0f - across / paint->vein_width);
				}
			}
			if (paint->mottle > 0.0f)
			{
				float grain = value_noise((float)x / 3.0f, (float)y / 3.0f) * 0.6f + value_noise((float)x / 9.0f, (float)y / 9.0f) * 0.4f;
				// (the side veins: fine lines slanting out towards the tip)
				float side = fabsf(sinf((u * 1.0f - fabsf(v) * 0.9f) / fmaxf(half_width, 1e-4f) * 9.0f));

				for (int k = 0; k < 3; k++)
				{
					color[k] *= 1.0f + paint->mottle * ((grain - 0.5f) * 0.9f + (side > 0.92f ? 0.25f : 0.0f));
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

// A cubic curve's point and heading (radians, 0 up the picture)
static void cubic_at(const float points[4][2], float t, float *x, float *y, float *heading)
{
	float u = 1.0f - t;
	float dx = 3.0f * u * u * (points[1][0] - points[0][0]) + 6.0f * u * t * (points[2][0] - points[1][0]) + 3.0f * t * t * (points[3][0] - points[2][0]);
	float dy = 3.0f * u * u * (points[1][1] - points[0][1]) + 6.0f * u * t * (points[2][1] - points[1][1]) + 3.0f * t * t * (points[3][1] - points[2][1]);

	*x = u * u * u * points[0][0] + 3.0f * u * u * t * points[1][0] + 3.0f * u * t * t * points[2][0] + t * t * t * points[3][0];
	*y = u * u * u * points[0][1] + 3.0f * u * u * t * points[1][1] + 3.0f * u * t * t * points[2][1] + t * t * t * points[3][1];
	*heading = atan2f(dx, -dy);
}

typedef struct
{
	float rachis[4][2];            // the frond's stalk, foot to tip (a cubic curve)
	float pinnae_from;             // along it, the lowest pinnae
	int pairs;                     // pinnae each side
	float pinna_length;            // the longest's
	float longest_at;              // along the pinnae, where they are longest (0: the lowest)
	float pinna_angle;             // out from the stalk, radians, at the foot ...
	float pinna_angle_tip;         // ... and at the tip
	float pinna_curl;              // a pinna curving towards the frond's tip
	float segment_length;          // a pinnule's length, of its pinna's length at its foot
	float segment_spacing;         // pinnules along a pinna, of their length
	float segment_angle;           // out from the pinna, radians
	float joined;                  // 0: the pinnules apart (twice divided); 1: lobes of one blade (once divided)
	int teeth;                     // on each side of a pinnule
	float tooth;
	float stalk_radius;
	blade_paint_t leaf;
	paint_t stalk;
} fern_style_t;

// A blade's paint a little brighter or darker (each pinnule its own)
static blade_paint_t shaded_paint(const blade_paint_t *paint, float brightness)
{
	blade_paint_t shaded = *paint;

	for (int k = 0; k < 3; k++)
	{
		shaded.base[k] *= brightness;
		shaded.vein[k] *= brightness;
		shaded.edge[k] *= brightness;
	}
	return shaded;
}

static void draw_pinna(canvas_t *canvas, random_t *random, const fern_style_t *style, float bx, float by, float angle, float length, float side)
{
	// (the smaller pinnae up the frond: their pinnules as big as they can
	// be, run together into lobes, as on the pictures: no sparse spikes)
	float smallness = clampf(1.0f - length / fmaxf(style->pinna_length, 1e-3f), 0.0f, 1.0f);
	float joined = mixf(style->joined, 1.0f, smallness * smallness);
	float segment = style->segment_length * length * mixf(1.0f, 1.5f, smallness);
	float step = segment * style->segment_spacing * mixf(1.0f, 0.85f, smallness);
	paint_t costa = { .tip_from = 1.0f };
	float t = step * 0.6f / length;

	// Its wing: the blade the pinnules sit on (narrow when they are apart,
	// broad when they are lobes of it)
	draw_blade(canvas, bx, by, angle, length, segment * mixf(0.18f, 0.55f, joined), side * style->pinna_curl, 0, 0.0f, 0.6f, &style->leaf);
	// Its pinnules both ways along it, shorter towards its tip
	while (t < 0.86f)
	{
		float curl_shift = side * style->pinna_curl * length * t * t * 0.5f;
		float px = bx + sinf(angle) * length * t + cosf(angle) * curl_shift;
		float py = by - cosf(angle) * length * t + sinf(angle) * curl_shift;
		float heading = angle + side * style->pinna_curl * t;
		float size = segment * (1.0f - 0.8f * t) * random_range(random, 0.92f, 1.06f);

		for (int k = -1; k <= 1; k += 2)
		{
			float out = heading + (float)k * style->segment_angle * random_range(random, 0.92f, 1.08f);
			float half = size * mixf(0.34f, 0.42f, joined);
			blade_paint_t own = shaded_paint(&style->leaf, random_range(random, 0.92f, 1.08f));

			draw_blade(canvas, px, py, out, size, half, (float)k * -0.25f, style->teeth, style->tooth, mixf(0.4f, 0.9f, joined), &own);
		}
		t += step * (1.0f - 0.55f * t) / length;
	}
	// The tip: one toothed blade where the pinnules run together
	draw_blade(canvas, bx + sinf(angle) * length * 0.72f + cosf(angle) * side * style->pinna_curl * length * 0.26f,
		by - cosf(angle) * length * 0.72f + sinf(angle) * side * style->pinna_curl * length * 0.26f,
		angle + side * style->pinna_curl * 0.72f, length * 0.36f, segment * 0.27f, side * style->pinna_curl * 0.6f, 6, style->tooth * 0.8f, 0.9f,
		&style->leaf);
	// Its midrib over them, a darker green than the stalk
	for (int k = 0; k < 3; k++)
	{
		costa.dark[k] = style->leaf.costa[k] * 0.7f;
		costa.light[k] = style->leaf.costa[k] * 1.15f;
	}
	draw_curve(canvas, bx, by, bx + sinf(angle) * length * 0.5f, by - cosf(angle) * length * 0.5f,
		bx + sinf(angle) * length * 0.95f + cosf(angle) * side * style->pinna_curl * length * 0.45f,
		by - cosf(angle) * length * 0.95f + sinf(angle) * side * style->pinna_curl * length * 0.45f,
		style->stalk_radius * 0.6f, style->stalk_radius * 0.12f, &costa);
}

static void draw_fern(canvas_t *canvas, random_t *random, const fern_style_t *style)
{
	float x;
	float y;
	float heading;
	const int pieces = 40;
	float last_x = style->rachis[0][0];
	float last_y = style->rachis[0][1];

	// The pinnae, the lowest first (those higher up over them, as on the
	// pictures), both sides a little apart along the stalk
	for (int i = 0; i < style->pairs; i++)
	{
		for (int k = -1; k <= 1; k += 2)
		{
			float place = ((float)i + (k > 0 ? 0.5f : 0.0f)) / (float)style->pairs;
			// (closer together towards the tip)
			float t = style->pinnae_from + (1.0f - style->pinnae_from) * powf(place, 0.78f) * 0.95f;
			float longest = style->longest_at;
			float size = place < longest ? mixf(0.82f, 1.0f, place / fmaxf(longest, 1e-3f))
				: mixf(1.0f, 0.12f, powf((place - longest) / (1.0f - longest), 1.05f));
			float out = mixf(style->pinna_angle, style->pinna_angle_tip, place);

			cubic_at(style->rachis, t, &x, &y, &heading);
			draw_pinna(canvas, random, style, x, y, heading + (float)k * out * random_range(random, 0.95f, 1.05f),
				style->pinna_length * size * random_range(random, 0.94f, 1.04f), -(float)k);
		}
	}
	// The stalk over them, thinning to the tip
	for (int p = 1; p <= pieces; p++)
	{
		float t = (float)p / (float)pieces;
		float t0 = (float)(p - 1) / (float)pieces;

		cubic_at(style->rachis, t, &x, &y, &heading);
		draw_segment(canvas, last_x, last_y, style->stalk_radius * mixf(1.0f, 0.15f, t0), x, y, style->stalk_radius * mixf(1.0f, 0.15f, t), t0, t,
			&style->stalk);
		last_x = x;
		last_y = y;
	}
	// The frond's own tip: a lobed blade over the stalk's last stretch
	// (the pinnae there run together into it)
	{
		float tip_x;
		float tip_y;
		float tip_heading;

		float end_x;
		float end_y;
		float end_heading;

		cubic_at(style->rachis, 0.88f, &tip_x, &tip_y, &tip_heading);
		cubic_at(style->rachis, 1.0f, &end_x, &end_y, &end_heading);
		draw_blade(canvas, tip_x, tip_y, tip_heading, 1.08f * sqrtf((end_x - tip_x) * (end_x - tip_x) + (end_y - tip_y) * (end_y - tip_y)),
			style->pinna_length * style->segment_length * 0.38f, 0.0f, 6, 0.65f, 0.7f, &style->leaf);
	}
}

// Twice divided, broad (the first reference: pinnae of separate toothed
// pinnules, the frond lanceolate, its tip leaning right)
static void draw_fern_broad(canvas_t *canvas, random_t *random)
{
	fern_style_t style = {
		.rachis = { { 0.58f, 1.98f }, { 0.42f, 1.35f }, { 0.52f, 0.5f }, { 0.86f, 0.04f } },
		.pinnae_from = 0.14f, .pairs = 12, .pinna_length = 0.5f, .longest_at = 0.12f,
		.pinna_angle = 1.15f, .pinna_angle_tip = 0.8f, .pinna_curl = 0.18f,
		.segment_length = 0.24f, .segment_spacing = 0.5f, .segment_angle = 1.0f, .joined = 0.15f,
		.teeth = 7, .tooth = 0.22f, .stalk_radius = 0.0085f,
		.leaf = { .base = { 0.31f, 0.46f, 0.12f }, .vein = { 0.19f, 0.32f, 0.07f }, .edge = { 0.22f, 0.36f, 0.09f }, .vein_width = 0.09f,
			.mottle = 0.22f, .costa = { 0.24f, 0.38f, 0.09f } },
		.stalk = { .dark = { 0.30f, 0.40f, 0.12f }, .light = { 0.55f, 0.65f, 0.24f }, .tip = { 0.40f, 0.52f, 0.16f }, .tip_from = 0.8f },
	};

	draw_fern(canvas, random, &style);
}

// Once divided and lobed (the second: pinnae cut into toothed lobes, the
// frond narrowing to a long tip curving left; darker, olive)
static void draw_fern_narrow(canvas_t *canvas, random_t *random)
{
	fern_style_t style = {
		.rachis = { { 0.47f, 1.98f }, { 0.46f, 1.4f }, { 0.66f, 0.7f }, { 0.27f, 0.03f } },
		.pinnae_from = 0.15f, .pairs = 12, .pinna_length = 0.46f, .longest_at = 0.08f,
		.pinna_angle = 1.38f, .pinna_angle_tip = 0.9f, .pinna_curl = 0.45f,
		.segment_length = 0.23f, .segment_spacing = 0.45f, .segment_angle = 0.95f, .joined = 0.55f,
		.teeth = 4, .tooth = 0.22f, .stalk_radius = 0.0085f,
		.leaf = { .base = { 0.33f, 0.42f, 0.11f }, .vein = { 0.22f, 0.29f, 0.07f }, .edge = { 0.26f, 0.33f, 0.08f }, .vein_width = 0.08f,
			.mottle = 0.2f, .costa = { 0.30f, 0.34f, 0.10f } },
		.stalk = { .dark = { 0.32f, 0.36f, 0.10f }, .light = { 0.55f, 0.58f, 0.22f }, .tip = { 0.40f, 0.44f, 0.14f }, .tip_from = 0.8f },
	};

	draw_fern(canvas, random, &style);
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
	// The clear rows over the twig cut off (a few kept): the picture as
	// tall as the twig, its foot where it was
	{
		int first = 0;

		while (first < height - 1)
		{
			bool empty = true;

			for (int x = 0; x < width && empty; x++)
			{
				empty = out[((size_t)first * (size_t)width + (size_t)x) * 4 + 3] == 0;
			}
			if (!empty)
			{
				break;
			}
			first++;
		}
		first = first > 4 ? first - 4 : 0;
		memmove(out, out + (size_t)first * (size_t)width * 4, (size_t)(height - first) * (size_t)width * 4);
		height -= first;
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
		fprintf(stderr, "Usage: potop_twig [--width W] [--seed N] pine|pine_brush|pine_rosette|pine_needles|juniper|fern|fern_lobed OUT.png\n");
		return 1;
	}
	// (as tall as the kind's twig is: the pine's branch tip square,
	// the juniper's twig twice as tall as wide)
	canvas.tall = strcmp(kind, "pine") == 0 ? 1.75f : (strcmp(kind, "pine_needles") == 0 ? 4.5f : 2.0f);
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
	else if (strcmp(kind, "pine_brush") == 0)
	{
		draw_pine_brush(&canvas, &random);
	}
	else if (strcmp(kind, "pine_rosette") == 0)
	{
		draw_pine_rosette(&canvas, &random);
	}
	else if (strcmp(kind, "pine_needles") == 0)
	{
		draw_pine_needles(&canvas, &random);
	}
	else if (strcmp(kind, "fern") == 0)
	{
		draw_fern_broad(&canvas, &random);
	}
	else if (strcmp(kind, "fern_lobed") == 0)
	{
		draw_fern_narrow(&canvas, &random);
	}
	else
	{
		fprintf(stderr, "potop_twig: no kind '%s' (pine, pine_brush, pine_rosette, pine_needles, juniper, fern, fern_lobed)\n", kind);
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
	printf("potop_twig: %s (%s, seed %u, %d wide)\n", path, kind, seed, width);
	return 0;
}
