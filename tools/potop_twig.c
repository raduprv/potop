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
	float *depth;                  // NULL, or each pixel's nearest depth so far (bigger: nearer); what is behind it is not drawn there
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

static float smooth_step(float low, float high, float value)
{
	float t = clampf((value - low) / (high - low), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
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

// Value noise in a few octaves (0..1, about 0.5 on average): finer
// detail on the coarse, each octave shifted so their lattices do not line up
static float fractal_noise(float x, float y, int octaves)
{
	float sum = 0.0f;
	float weight = 0.5f;
	float total = 0.0f;

	for (int i = 0; i < octaves; i++)
	{
		sum += value_noise(x + 17.3f * (float)i, y - 31.7f * (float)i) * weight;
		total += weight;
		x = x * 2.03f + 5.1f;
		y = y * 1.97f - 3.7f;
		weight *= 0.5f;
	}
	return sum / total;
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
	// Two toned: the half facing the light this color (a spruce's four
	// sided needle: its lit face yellow green, a sharp ridge along its
	// middle, the other face darker), 0: one color all round
	float two_tone;
	float light_side[3];
	bool two_tone_fixed;           // the light half always the needle's right (its own color, not the light's: no light baked in)
	float hue_spread;              // how far each needle's own hue strays (yellower or bluer), on top of the usual (0: the usual)
	float round_shade;             // darker towards its edges, lightest along its middle (its roundness, the same whatever the light; 0: none)
	float foot_shade;              // darker towards its foot, in among the others by the shoot, lighter out at its tip (0: none)
	float flecks;                  // fine pale specks in lines along it (stomata, a waxy bloom), sharp, a pixel or two each (0: none)
	float fleck_color[3];
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
						base += hue_shift * (k == 0 ? 0.06f : (k == 1 ? 0.03f : -0.03f)) * (shape->two_tone > 0.0f ? 1.4f : 1.0f) * (1.0f + shape->hue_spread);
						if (along > dry)
						{
							float brown[3] = { 0.44f, 0.32f, 0.19f };

							base = mixf(base, brown[k], clampf((along - dry) / 0.06f, 0.0f, 1.0f));
						}
						color[k] = base * lit * brightness;
					}
					if (shape->flecks > 0.0f)
					{
						// (in a few lines along it, each broken into short dashes)
						float line = fabsf(sinf((across + 1.0f) * 3.3f * PI_F + grain_offset));
						float dash = value_noise(across * 9.0f + grain_offset * 3.0f, along * total / 2.2f);
						float speck = smooth_step(0.78f, 0.95f, line) * smooth_step(0.42f, 0.6f, dash) * shape->flecks;

						for (int k = 0; k < 3; k++)
						{
							color[k] = mixf(color[k], shape->fleck_color[k] * brightness, clampf(speck, 0.0f, 1.0f));
						}
					}
					if (shape->foot_shade > 0.0f)
					{
						float out_share = sqrtf(clampf(along, 0.0f, 1.0f));

						for (int k = 0; k < 3; k++)
						{
							color[k] *= mixf(1.0f - shape->foot_shade, 1.0f + 0.3f * shape->foot_shade, out_share);
						}
					}
					if (shape->round_shade > 0.0f)
					{
						float middle = sqrtf(fmaxf(1.0f - across * across, 0.0f));

						for (int k = 0; k < 3; k++)
						{
							color[k] *= mixf(1.0f - shape->round_shade, 1.0f + 0.25f * shape->round_shade, middle);
						}
					}
					if (shape->two_tone > 0.0f)
					{
						float lit_half = clampf((across * (shape->two_tone_fixed ? 1.0f : facing) + 0.1f) / 0.2f, 0.0f, 1.0f);

						for (int k = 0; k < 3; k++)
						{
							float light = shape->light_side[k] + hue_shift * (k == 0 ? 0.08f : (k == 1 ? 0.04f : -0.03f));

							// (towards its tip the tip's color on that side too)
							if (along > shape->tip_from)
							{
								light = mixf(light, shape->tip[k] * 1.1f, (along - shape->tip_from) / fmaxf(1.0f - shape->tip_from, 1e-3f));
							}

							color[k] = mixf(color[k] * (1.0f - 0.15f * shape->two_tone), light * brightness * mixf(0.85f, 1.0f, lit),
								lit_half * shape->two_tone);
						}
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
	float new_growth;              // the needles near each shoot's tip this much yellower (this year's growth; 0: none)
	float flatten;                 // the needles pushed out to the shoot's two sides (seen from above, a comb; 0: all round it)
	float front_from;              // only the needles this far towards us drawn over the shoots (0: all those in front of them)
	float curl_outward;            // the needles away from a shoot's tip curve out (this much of curl; 0: all curve in), those at its tip in round the bud
	float tip_shorter;             // the needles at a shoot's very tip this share shorter (0: as long as the rest)
	bool alternate;                // each next needle on the other side of the shoot (the spiral seen side on: never two in a row on one side)
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
			if (style->alternate && ((sinf(around) >= 0.0f) != (bundle_index % 2 == 0)))
			{
				// (mirrored to the side it is due on)
				around = -around;
			}
			if (style->flatten > 0.0f)
			{
				// (pushed out to the sides: fewer towards us and away)
				around = atan2f(sinf(around), cosf(around) * (1.0f - style->flatten));
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
				* random_range(random, 0.94f, 1.06f), style->order) * scale * (style->tip_shorter > 0.0f ? 0.92f : mixf(0.8f, 1.05f, t))
				* (1.0f - 0.55f * fabsf(cosf(around)) * sinf(fminf(spread / (0.55f + 0.45f * fabsf(side)), 1.5f)));
			needle->curl = (side >= 0.0f ? -1.0f : 1.0f) * style->curl * mixf(random_range(random, 0.5f, 1.5f), random_range(random, 0.85f, 1.15f), style->order)
				+ random_range(random, -0.5f, 0.5f) * style->unevenness;
			if (style->tip_shorter > 0.0f)
			{
				// (shorter towards the tip: the youngest)
				float near_tip = clampf((t - 0.55f) / 0.45f, 0.0f, 1.0f);

				needle->length *= 1.0f - style->tip_shorter * near_tip * near_tip;
			}
			if (style->curl_outward > 0.0f)
			{
				// (out along the shoot, turning in towards its tip, round the bud)
				float inward = clampf((t - 0.72f) / 0.2f, 0.0f, 1.0f);

				needle->curl *= mixf(-style->curl_outward, 1.6f, inward);
			}
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
			if (style->new_growth > 0.0f)
			{
				// (this year's growth at the tip: yellower, lighter)
				float young = clampf((t - 0.72f) / 0.22f, 0.0f, 1.0f);

				needle->hue = fminf(needle->hue + young * young * style->new_growth, 2.2f);
			}
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
		needle->length = random_range(random, style->length_low, style->length_high) * scale * 0.9f * (1.0f - style->tip_shorter);
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

		if ((needle->depth >= style->front_from) != front)
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

// A spruce's (fir's) spray: a stem up the middle, a few shoots out of it
// alternately both ways, the lower longer, all of them clothed to their
// tips in short stiff sharp needles, one by one in a spiral round the
// shoot, pointing forward (a bottle brush seen side on); dark green, this
// year's growth at the tips yellower; the orange brown shoot and its pegs
// showing between them; a bare woody stub at the foot
static void draw_spruce(canvas_t *canvas, random_t *random)
{
	static needle_shape_t needle;
	needle_style_t style = {
		.length_low = 0.13f, .length_high = 0.155f, .radius = 0.0054f,
		.angle_low = 0.95f, .angle_high = 1.15f, .curl = 0.3f,
		.per_bundle = 1, .bundles_per_unit = 157.5f, .crowding = 0.65f, .bare = 0.0f, .brush = 0.15f,
		.greens = {
			{ .dark = { 0.08f, 0.17f, 0.06f }, .light = { 0.30f, 0.48f, 0.18f }, .tip = { 0.38f, 0.52f, 0.18f }, .tip_from = 1.0f },
			{ .dark = { 0.15f, 0.22f, 0.05f }, .light = { 0.48f, 0.58f, 0.20f }, .tip = { 0.52f, 0.60f, 0.20f }, .tip_from = 1.0f } },
		.behind_dark = 0.45f, .spread_taper = 0.7f, .brightness_spread = 0.32f,
		.pairs_together = false, .rim = 0.2f, .gloss = 0.0f, .shadow = 0.0f, .tip_radius = 0.3f,
		.unevenness = 0.2f, .dry_share = 0.01f, .broken_share = 0.01f, .order = 1.0f, .new_growth = 0.8f, .flatten = 0.5f, .front_from = 0.0f, .curl_outward = 0.7f, .tip_shorter = 0.6f, .alternate = true,
	};
	// (four sided, so a light ridge along its middle, sharp, a little
	// narrower at its foot; dark green, yellowish at its tip)
	needle = (needle_shape_t){
		.foot_narrow = 0.35f, .tip_round = 0.55f, .tip_length = 0.2f, .flat = 0.0f,
		.foot = { 0.26f, 0.31f, 0.13f }, .middle = { 0.15f, 0.27f, 0.10f }, .tip = { 0.62f, 0.64f, 0.18f },
		.foot_to = 0.1f, .tip_from = 0.9f,
		.stripes = 0.55f, .stripe_color = { 0.40f, 0.50f, 0.25f },
		.streaks = 1.2f, .gloss = 0.0f, .shade = 0.0f,
		.two_tone = 0.55f, .light_side = { 0.34f, 0.44f, 0.17f }, .two_tone_fixed = true, .hue_spread = 1.5f, .round_shade = 0.55f, .foot_shade = 0.55f, .flecks = 1.0f, .fleck_color = { 0.68f, 0.74f, 0.46f },
	};
	style.shape = &needle;
	// (the shoots orange brown with pegs where the needles stand; the stub
	// greyer, older)
	paint_t bark = { .dark = { 0.26f, 0.12f, 0.05f }, .light = { 0.70f, 0.42f, 0.20f }, .tip = { 0.55f, 0.36f, 0.18f }, .tip_from = 0.6f,
		.rough = 0.35f, .bumps = 1.0f };
	paint_t stub_bark = { .dark = { 0.24f, 0.17f, 0.11f }, .light = { 0.66f, 0.52f, 0.38f }, .tip = { 0.55f, 0.38f, 0.22f }, .tip_from = 0.7f,
		.rough = 0.8f, .bumps = 0.6f };
	paint_t bud = { .dark = { 0.25f, 0.15f, 0.07f }, .light = { 0.62f, 0.44f, 0.24f }, .tip = { 0.60f, 0.46f, 0.28f }, .tip_from = 0.7f,
		.rough = 0.8f };
	shoot_t shoots[8];
	int count = 0;
	int sides = 4 + (random_float(random) < 0.3f ? 1 : 0);
	float first_side = random_float(random) < 0.5f ? 1.0f : -1.0f;
	float stem_length = canvas->tall * 0.64f;
	float stub = 0.07f;
	float x;
	float y;
	float heading;

	// The stub at the foot, then the stem on from it
	shoots[count++] = (shoot_t){ .fx = 0.5f, .fy = canvas->tall - 0.02f, .angle = random_range(random, -0.06f, 0.06f), .bend = 0.0f,
		.length = stem_length * stub, .radius = 0.02f, .scale = 1.0f, .taper = 0.85f, .bare = true, .crook = 0.04f,
		.phase = random_range(random, 0.0f, 6.0f), .bark = &stub_bark };
	shoot_point(&shoots[0], 1.0f, &x, &y, &heading);
	shoots[count++] = (shoot_t){ .fx = x, .fy = y, .angle = heading, .bend = random_range(random, -0.15f, 0.15f),
		.length = stem_length * (1.0f - stub), .radius = 0.017f, .scale = 0.78f, .taper = 0.4f, .needles_from = 0.01f, .crook = 0.07f,
		.phase = random_range(random, 0.0f, 6.0f) };
	// The shoots, alternately, the lower longer, out and a little up
	for (int i = 0; i < sides && count < 8; i++)
	{
		// (the lowest one small, right at the foot; the rest spread above it)
		float along = i == 0 ? random_range(random, 0.01f, 0.04f)
			: 0.2f + 0.62f * ((float)(i - 1) + random_range(random, 0.0f, 0.35f)) / (float)(sides - 1);
		float side = (i % 2 == 0) ? first_side : -first_side;
		float length = random_range(random, 0.4f, 0.46f) * (1.0f - 0.45f * along) * stem_length * (i == 0 ? random_range(random, 0.42f, 0.52f) : 1.0f);

		shoot_point(&shoots[1], along, &x, &y, &heading);
		shoots[count++] = (shoot_t){ .fx = x, .fy = y, .angle = heading + side * random_range(random, 0.46f, 0.54f),
			.bend = side * random_range(random, 0.08f, 0.16f), .length = length, .radius = 0.012f, .scale = 1.08f, .taper = 0.5f,
			.needles_from = 0.02f, .crook = 0.16f, .phase = random_range(random, 0.0f, 6.0f) };
	}
	draw_twig(canvas, random, &style, &bark, NULL, shoots, count);
	(void)bud;
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

// --- A berry shrub's twig (cranberry, lingonberry) ------------------------------------------

// A leaf's colors: its blade, its midrib, its rolled rim, and how uneven
typedef struct
{
	float blade[3];
	float midrib[3];
	float rim[3];
	float young[3];                // a young leaf's (the shoot's tip)
	float under[3];                // its underside, paler (a leaf turned over shows it)
	float mottle;
	float pointed;                 // 0: round at the tip (lingonberry); 1: drawn to a point (blueberry)
	float veins;                   // how deep its side veins show (0: as lingonberry's, faint)
	float widest;                  // where along the blade it is widest (0: at 55%)
	float slender;                 // its width times this (0: as drawn, 1)
	float smooth;                  // its rim's evenness and its blade's flatness (0: lingonberry's; 1: even, flat)
	float matte;                   // 0: glossy (lingonberry); 1: dull, a faint grey bloom over it, no sheen
	float stalk;                   // its stalk's length, of the leaf's (0: 0.1)
	float net;                     // a fine pale net of veins between the side veins (0: none, a faint dark one)
} oval_leaf_paint_t;

// What lies under a thing about to be drawn, darkened round it (not
// light from a side: as much all round, the closeness of one thing over
// another): within reach of the outline's distance function. distance:
// how far a pixel is outside the thing (units, below 0 inside)
static void darken_under(float *pixel, float outside, float reach, float strength)
{
	float amount = strength * clampf(1.0f - outside / reach, 0.0f, 1.0f);

	amount *= amount;
	for (int k = 0; k < 3; k++)
	{
		pixel[k] *= 1.0f - amount;
	}
}

// An oval leaf from (bx, by) heading at angle (radians, 0 up the picture,
// positive to the right): a short stalk, then a blade widest past its
// middle and round at the tip (obovate), length long, half_width wide each
// side, bent by curl; its rim rolled under (darker), a midrib along it,
// the side veins faint. seen: 1 face on, less turned away (narrower, as
// it is seen edge on); young: how much of the young leaf's color. Its
// color is its own, no light baked in: only a little darker at its rim
// and in its fold, the same whatever the light
static void draw_oval_leaf_part(canvas_t *canvas, float bx, float by, float angle, float length, float half_width, float curl, float seen,
	float young, float brightness, float turned, const oval_leaf_paint_t *paint, int part, float depth_foot, float depth_tip)
{
	float s = canvas->scale;
	float reach = (length + half_width) * s + 2.0f;
	int left = (int)floorf(bx * s - reach);
	int right = (int)ceilf(bx * s + reach);
	int top = (int)floorf(by * s - reach);
	int bottom = (int)ceilf(by * s + reach);
	float along_x = sinf(angle);
	float along_y = -cosf(angle);
	const float stalk = paint->stalk > 0.0f ? paint->stalk : 0.1f;
	// (a smooth leaf's midrib bowed a little, its own way)
	float rib_bow = (hash_unit((int)(bx * 733.0f), (int)(by * 1559.0f)) - 0.5f) * 0.07f * paint->smooth;
	float widest = paint->widest > 0.0f ? paint->widest : 0.55f;
	float width_seen;

	half_width *= paint->slender > 0.0f ? paint->slender : 1.0f;
	width_seen = half_width * clampf(seen, 0.25f, 1.0f);
	// (its own: where its marks are, which half is paler and how much)
	float seed = hash_unit((int)(bx * 1931.0f), (int)(by * 1277.0f)) * 100.0f;
	float half_tone = (hash_unit((int)(by * 811.0f), (int)(bx * 557.0f)) - 0.5f) * 0.3f;

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
			float v = px * -along_y + py * along_x;
			float t;
			float width;
			float across;
			float tip_point = 0.0f;
			float coverage;
			float color[3];
			float *pixel;

			if (u < 0.0f || u > length)
			{
				continue;
			}
			t = u / length;
			v -= curl * length * t * t * 0.5f;
			// (part 1: only its stalk, part 2: only its blade, else both)
			if ((part == 1 && t >= stalk) || (part == 2 && t < stalk))
			{
				continue;
			}
			// (with depths: not where something nearer is already; its
			// depth from its foot to its tip)
			if (canvas->depth != NULL && mixf(depth_foot, depth_tip, t) < canvas->depth[(size_t)y * (size_t)canvas->width + (size_t)x])
			{
				continue;
			}
			if (t < stalk)
			{
				// (the stalk: a thin green line)
				width = half_width * 0.07f;
			}
			else
			{
				// (the blade: an egg, widest at 60%, round at both ends)
				float b = (t - stalk) / (1.0f - stalk);
				// (an ellipse widest a little past the middle: the foot a
				// wedge into the stalk, the tip round)
				float e = b < widest ? b / widest : 1.0f - (b - widest) / (1.0f - widest);
				float round = b < widest ? powf(sinf(e * PI_F * 0.5f), mixf(0.9f, 0.75f, paint->smooth))
					: mixf(sqrtf(fmaxf(1.0f - (1.0f - e) * (1.0f - e), 0.0f)), powf(sinf(e * PI_F * 0.5f), 1.1f), paint->pointed);

				width = fmaxf(width_seen * round, half_width * 0.07f);
				// (past its middle, where the blade has run out: a tiny
				// point, tapering to nothing; a smooth leaf's red brown)
				if (b > 0.5f && width_seen * round < half_width * 0.06f)
				{
					width = half_width * 0.06f * clampf((1.0f - b) / 0.025f, 0.0f, 1.0f);
					tip_point = 1.0f;
				}
			}
			// (its rim a little uneven)
			if (t >= stalk)
			{
				width *= 1.0f + (fractal_noise(u / length * 30.0f + seed, (v > 0.0f ? 7.0f : -7.0f) + seed, 2) - 0.5f) * 0.08f * (1.0f - 0.75f * paint->smooth);
			}
			coverage = clampf((width - fabsf(v)) * s + 0.5f, 0.0f, 1.0f);
			// (what lies under it darkened a little round it and under it)
			if (t >= stalk && fabsf(v) - width < half_width * 0.35f)
			{
				darken_under(&canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4], fmaxf(fabsf(v) - width, 0.0f),
					half_width * 0.35f, 0.42f);
			}
			if (coverage <= 0.0f)
			{
				continue;
			}
			across = fabsf(v) / fmaxf(width, 1e-5f);
			{
				// Its own marks, scaled with it: the grain, the side veins
				// sunk in (darker lines, the fields between them a little
				// raised, paler), the fine net between those, one half a
				// little paler than the other (folded along the midrib),
				// paler towards the tip, a thin pale line inside the rolled
				// rim
				float lu = u / length;
				// (across from the midrib, bowed)
				float v_rib = v - rib_bow * length * sinf(PI_F * clampf((t - stalk) / (1.0f - stalk), 0.0f, 1.0f));
				float lv = v_rib / length;
				float grain = fractal_noise(lu * 60.0f + seed, lv * 60.0f, 3);
				float blotch = fractal_noise(lu * 7.0f + seed * 2.0f, lv * 7.0f, 3);
				float wobble = (fractal_noise(lu * 9.0f + seed, lv * 9.0f, 2) - 0.5f) * 1.2f;
				float vein_phase = (lu - fabsf(lv) * 1.6f) * 26.0f + wobble;
				float vein_line = smooth_step(mixf(0.86f, 0.955f, paint->smooth), mixf(0.98f, 0.985f, paint->smooth), fabsf(sinf(vein_phase)))
					* clampf((0.85f - across) / 0.3f, 0.0f, 1.0f);
				float vein_field = 0.5f + 0.5f * cosf(vein_phase * 2.0f);
				float net = 1.0f - fabsf(2.0f * fractal_noise(lu * 34.0f + seed, lv * 34.0f, 2) - 1.0f);
				// (the fine net: veins off the side veins, wandering, joining)
				float fine = 1.0f - fabsf(2.0f * fractal_noise(lu * 20.0f + seed * 3.0f, lv * 20.0f + wobble * 0.3f, 2) - 1.0f);
				float net_line = smooth_step(0.945f, 0.985f, fine) * clampf((0.9f - across) / 0.2f, 0.0f, 1.0f) * paint->net;
				float half = v > 0.0f ? half_tone : -half_tone;
				float inner_rim = clampf(1.0f - fabsf(across - 0.86f) / 0.05f, 0.0f, 1.0f);

				for (int k = 0; k < 3; k++)
				{
					float base = mixf(mixf(paint->blade[k], paint->young[k], young), paint->under[k], turned);

					color[k] = base;
					if (t >= stalk)
					{
						// (a smooth leaf's midrib a sharp line, thinning to the tip)
						float rib = mixf(clampf(1.0f - across / 0.06f, 0.0f, 1.0f),
							1.0f - smooth_step(0.025f, 0.045f, fabsf(v_rib) / half_width / mixf(1.0f, 0.4f, t)), paint->smooth)
							* clampf((0.97f - t) / 0.2f, 0.0f, 1.0f);
						float fold = clampf(1.0f - fabsf(across - 0.12f) / 0.08f, 0.0f, 1.0f) * 0.2f;
						// (round across: its middle raised, its sides curving
						// down to the rolled rim, darker)
						float round_across = mixf(0.88f, 0.94f, paint->smooth) + mixf(0.22f, 0.1f, paint->smooth) * (1.0f - 0.5f * paint->matte)
							* sqrtf(fmaxf(1.0f - across * across, 0.0f));
						float shade = round_across * (1.0f + half * smooth_step(0.05f, 0.5f, across) + (lu - 0.5f) * 0.1f
							+ (grain - 0.5f) * paint->mottle * 0.5f + (blotch - 0.5f) * paint->mottle * 0.8f
							- vein_line * 0.06f * (1.0f - paint->veins) + (vein_field - 0.5f) * 0.03f * (1.0f - paint->smooth)
							- smooth_step(0.93f, 1.0f, net) * (0.04f + 0.08f * paint->veins) * (1.0f - paint->net) - fold);

						// (its waxy sheen: a paler, greyer patch on its paler
						// half, off the midrib, broken up; its own, not a
						// light's)
						float sheen = (v * half_tone > 0.0f ? 1.0f : 0.25f) * smooth_step(0.08f, 0.25f, across) * (1.0f - smooth_step(0.55f, 0.8f, across))
							* smooth_step(0.2f, 0.4f, lu) * (1.0f - smooth_step(0.75f, 0.95f, lu)) * smooth_step(0.35f, 0.65f, blotch);

						color[k] = base * shade;
						color[k] = mixf(color[k], color[k] * 0.6f + paint->midrib[k] * 0.55f + 0.04f, sheen * 0.45f * (1.0f - paint->matte));
						color[k] = mixf(color[k], paint->midrib[k], rib * mixf(0.75f, 0.95f, paint->smooth));
						// (the side veins pale, raised: as pale as the midrib
						// as much as veins says)
						color[k] = mixf(color[k], paint->midrib[k] * 1.05f, vein_line * paint->veins);
						color[k] = mixf(color[k], paint->midrib[k], net_line * 0.35f);
						// (a smooth leaf's rim not rolled: no pale line inside
						// it, just a thin darker edge)
						color[k] = mixf(color[k], paint->midrib[k] * 1.15f, inner_rim * 0.35f * (1.0f - paint->smooth));
						color[k] = mixf(color[k], paint->rim[k], smooth_step(mixf(0.92f, 0.96f, paint->smooth), 1.0f, across)
							* mixf(0.85f, 0.6f, paint->matte));
						// (the bloom: a little grey over all of it, evened out)
						color[k] = mixf(color[k], (color[k] + 0.30f) * 0.5f * (0.9f + 0.2f * blotch), paint->matte * 0.18f);
						{
							const float point_color[3] = { 0.46f, 0.22f, 0.13f };

							color[k] = mixf(color[k], point_color[k], tip_point * paint->smooth);
						}
					}
				}
			}
			pixel = &canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4];
			for (int k = 0; k < 3; k++)
			{
				pixel[k] = color[k] * brightness * coverage + pixel[k] * (1.0f - coverage);
			}
			pixel[3] = coverage + pixel[3] * (1.0f - coverage);
			if (canvas->depth != NULL && coverage >= 0.5f)
			{
				canvas->depth[(size_t)y * (size_t)canvas->width + (size_t)x] = mixf(depth_foot, depth_tip, t);
			}
		}
	}
}

static void draw_oval_leaf(canvas_t *canvas, float bx, float by, float angle, float length, float half_width, float curl, float seen,
	float young, float brightness, float turned, const oval_leaf_paint_t *paint)
{
	draw_oval_leaf_part(canvas, bx, by, angle, length, half_width, curl, seen, young, brightness, turned, paint, 0, 0.0f, 0.0f);
}

// Leaves queued up and drawn together. Without depths: all their stalks
// first, then all their blades, so no stalk ever lies over another leaf.
// With them (over wood): each where it is nearest, the wood at depth 0
typedef struct
{
	float x;
	float y;
	float angle;
	float length;
	float half_width;
	float curl;
	float seen;
	float young;
	float brightness;
	float turned;
	const oval_leaf_paint_t *paint;
	float depth_tip;               // its tip's depth, its foot near the wood's (0: none given)
	float bud_angle;               // the little bud in its axil, over its foot (bud_length 0: none)
	float bud_length;
} queued_leaf_t;

#define LEAF_QUEUE_SIZE 256

static queued_leaf_t leaf_queue[LEAF_QUEUE_SIZE];
static int leaf_queue_count;
// (points along the shoots the queued leaves grow on: a leaf hiding one
// is sent behind the wood)
#define WOOD_SAMPLES_SIZE 512

static float wood_samples[WOOD_SAMPLES_SIZE][2];
static int wood_sample_count;

static void queue_oval_leaf(canvas_t *canvas, float bx, float by, float angle, float length, float half_width, float curl, float seen,
	float young, float brightness, float turned, const oval_leaf_paint_t *paint, float depth_tip)
{
	if (leaf_queue_count >= LEAF_QUEUE_SIZE)
	{
		draw_oval_leaf(canvas, bx, by, angle, length, half_width, curl, seen, young, brightness, turned, paint);
		return;
	}
	leaf_queue[leaf_queue_count++] = (queued_leaf_t){ bx, by, angle, length, half_width, curl, seen, young, brightness, turned, paint, depth_tip, 0.0f, 0.0f };
}

// The queued leaves drawn. wood: NULL, or the wood drawn on its own layer:
// put down first, at depth 0, the leaves then each in front of it or
// behind as its depth says (and of each other)
static void draw_bud(canvas_t *canvas, float x0, float y0, float angle, float length, float shade);

// Whether a queued leaf's blade covers (x, y), near enough: an oval along
// it, bent as it is
static bool leaf_covers(const queued_leaf_t *leaf, float x, float y)
{
	float px = x - leaf->x;
	float py = y - leaf->y;
	float u = px * sinf(leaf->angle) - py * cosf(leaf->angle);
	float v = px * cosf(leaf->angle) + py * sinf(leaf->angle);
	float t = u / leaf->length;

	if (t < 0.05f || t > 0.98f)
	{
		return false;
	}
	v -= leaf->curl * leaf->length * t * t * 0.5f;
	return fabsf(v) < leaf->half_width * clampf(leaf->seen, 0.25f, 1.0f) * sinf(PI_F * t) * 0.95f;
}

static void flush_oval_leaves(canvas_t *canvas, const canvas_t *wood)
{
	size_t count = (size_t)canvas->width * (size_t)canvas->height;

	if (wood != NULL)
	{
		canvas->depth = malloc(sizeof(float) * count);
	}
	if (canvas->depth == NULL)
	{
		for (int part = 1; part <= 2; part++)
		{
			for (int i = 0; i < leaf_queue_count; i++)
			{
				const queued_leaf_t *leaf = &leaf_queue[i];

				draw_oval_leaf_part(canvas, leaf->x, leaf->y, leaf->angle, leaf->length, leaf->half_width, leaf->curl, leaf->seen,
					leaf->young, leaf->brightness, leaf->turned, leaf->paint, part, 0.0f, 0.0f);
			}
		}
		leaf_queue_count = 0;
		return;
	}
	for (size_t i = 0; i < count; i++)
	{
		const float *source = &wood->pixels[i * 4];
		float *pixel = &canvas->pixels[i * 4];

		for (int k = 0; k < 4; k++)
		{
			pixel[k] = source[k] + pixel[k] * (1.0f - source[3]);
		}
		canvas->depth[i] = source[3] >= 0.5f ? 0.0f : -1e9f;
	}
	// Each leaf flat at its own depth (none through another), in front of
	// the wood or behind it; but one whose blade would hide another's foot
	// sent behind that one and behind the wood, so the foot shows on the
	// wood it grows from, and no leaf hangs off nothing (a few rounds: one
	// sent back may now be behind another that hides a foot)
	{
		float depth[LEAF_QUEUE_SIZE];

		for (int i = 0; i < leaf_queue_count; i++)
		{
			depth[i] = leaf_queue[i].depth_tip;
		}
		for (int round = 0; round < 8; round++)
		{
			bool changed = false;

			for (int i = 0; i < leaf_queue_count; i++)
			{
				const queued_leaf_t *leaf = &leaf_queue[i];
				// (its foot: just past its stalk, where it meets the shoot)
				float foot_x = leaf->x + sinf(leaf->angle) * leaf->length * 0.06f;
				float foot_y = leaf->y - cosf(leaf->angle) * leaf->length * 0.06f;

				for (int j = 0; j < leaf_queue_count; j++)
				{
					if (j != i && depth[j] >= depth[i] && leaf_covers(&leaf_queue[j], foot_x, foot_y))
					{
						depth[j] = fminf(depth[i], 0.0f) - 0.01f;
						changed = true;
					}
				}
			}
			// (nor a shoot hidden under leaves in front of it: those over
			// it, away from their own foot, behind it)
			for (int j = 0; j < leaf_queue_count; j++)
			{
				const queued_leaf_t *leaf = &leaf_queue[j];

				if (depth[j] < 0.0f)
				{
					continue;
				}
				for (int k = 0; k < wood_sample_count; k++)
				{
					float dx = wood_samples[k][0] - leaf->x;
					float dy = wood_samples[k][1] - leaf->y;

					if (dx * dx + dy * dy > leaf->length * leaf->length * 0.09f && leaf_covers(leaf, wood_samples[k][0], wood_samples[k][1]))
					{
						depth[j] = -fabsf(depth[j]) - 0.01f;
						changed = true;
						break;
					}
				}
			}
			if (!changed)
			{
				break;
			}
		}
		for (int i = 0; i < leaf_queue_count; i++)
		{
			const queued_leaf_t *leaf = &leaf_queue[i];

			draw_oval_leaf_part(canvas, leaf->x, leaf->y, leaf->angle, leaf->length, leaf->half_width, leaf->curl, leaf->seen, leaf->young,
				leaf->brightness, leaf->turned, leaf->paint, 0, depth[i], depth[i]);
		}
	}
	// (the buds in the leaves' axils last, over their feet: not where
	// another leaf hides that foot)
	{
		float *depth = canvas->depth;

		canvas->depth = NULL;
		for (int i = 0; i < leaf_queue_count; i++)
		{
			const queued_leaf_t *leaf = &leaf_queue[i];
			int x = (int)(leaf->x * canvas->scale);
			int y = (int)(leaf->y * canvas->scale);

			if (leaf->bud_length > 0.0f && x >= 0 && y >= 0 && x < canvas->width && y < canvas->height
				&& depth[(size_t)y * (size_t)canvas->width + (size_t)x] <= 0.05f)
			{
				draw_bud(canvas, leaf->x, leaf->y, leaf->bud_angle, leaf->bud_length, leaf->brightness);
			}
		}
		free(depth);
	}
	leaf_queue_count = 0;
	wood_sample_count = 0;
}

// A berry at (cx, cy), radius long, its stalk's end towards -end and its
// foot towards end (a unit vector): a little flattened along that way,
// darker towards its rim (no light baked in) and towards its stalk, a
// paler pinker side of its own as it ripens unevenly, its skin's fine pale
// dots and a few dark blemishes, a dimple where the stalk goes in, and at
// its foot the dry flower: a little crown of five brown points
static void draw_berry(canvas_t *canvas, float cx, float cy, float radius, float end_x, float end_y, const float color[3], const float rim[3],
	float brightness)
{
	float s = canvas->scale;
	float reach = radius * 1.4f;
	int left = (int)floorf((cx - reach) * s - 2.0f);
	int right = (int)ceilf((cx + reach) * s + 2.0f);
	int top = (int)floorf((cy - reach) * s - 2.0f);
	int bottom = (int)ceilf((cy + reach) * s + 2.0f);
	// (its own: from its place)
	float h1 = hash_unit((int)(cx * 977.0f), (int)(cy * 613.0f));
	float h2 = hash_unit((int)(cy * 433.0f), (int)(cx * 271.0f));
	float h3 = hash_unit((int)(cx * 157.0f) + 7, (int)(cy * 919.0f));
	float flat = 0.9f + 0.08f * h3;
	float pale_angle = h1 * 2.0f * PI_F;
	float pale_x = cx + cosf(pale_angle) * radius * 0.45f;
	float pale_y = cy + sinf(pale_angle) * radius * 0.45f;
	float pale_share = 0.25f + 0.45f * h2;
	const float pale[3] = { 0.96f, 0.50f, 0.48f };
	const float calyx_dark[3] = { 0.14f, 0.07f, 0.04f };
	const float calyx_light[3] = { 0.42f, 0.28f, 0.16f };
	float crown_x = cx + end_x * radius * flat * 0.93f;
	float crown_y = cy + end_y * radius * flat * 0.93f;
	float crown_turn = h3 * 2.0f * PI_F;
	float dimple_x = cx - end_x * radius * flat * 0.88f;
	float dimple_y = cy - end_y * radius * flat * 0.88f;

	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right >= canvas->width ? canvas->width - 1 : right;
	bottom = bottom >= canvas->height ? canvas->height - 1 : bottom;
	for (int y = top; y <= bottom; y++)
	{
		for (int x = left; x <= right; x++)
		{
			float px = ((float)x + 0.5f) / s;
			float py = ((float)y + 0.5f) / s;
			// (along its stalk's way, across it: flattened along)
			float along = ((px - cx) * end_x + (py - cy) * end_y) / flat;
			float side = -(px - cx) * end_y + (py - cy) * end_x;
			float distance = sqrtf(along * along + side * side);
			float coverage = clampf((radius - distance) * s + 0.5f, 0.0f, 1.0f);
			float r = distance / radius;
			// The crown at its foot: five points round a hollow, past the
			// rim where the foot is seen side on
			float crown_dx = px - crown_x;
			float crown_dy = py - crown_y;
			float crown_distance = sqrtf(crown_dx * crown_dx + crown_dy * crown_dy);
			float crown_angle = atan2f(crown_dy, crown_dx) + crown_turn;
			float point = powf(fabsf(cosf(crown_angle * 2.5f)), 3.0f);
			float crown_radius = radius * (0.1f + 0.13f * point);
			float crown = clampf((crown_radius - crown_distance) * s + 0.5f, 0.0f, 1.0f);
			float dimple = clampf(1.0f - sqrtf((px - dimple_x) * (px - dimple_x) + (py - dimple_y) * (py - dimple_y)) / (radius * 0.16f), 0.0f, 1.0f);
			float speck = 0.0f;
			float blemish = 0.0f;
			float color_here[3];
			float *pixel;

			if (distance - radius < radius * 0.3f)
			{
				darken_under(&canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4], fmaxf(distance - radius, 0.0f), radius * 0.3f,
					0.45f);
			}
			if (coverage <= 0.0f && crown <= 0.0f)
			{
				continue;
			}
			// (its skin's fine pale dots, some drawn out a little; now and
			// then a dark blemish)
			{
				float gx = side / radius * 11.0f;
				float gy = along / radius * 11.0f;
				int cell_x = (int)floorf(gx);
				int cell_y = (int)floorf(gy);
				int key = (int)(cx * 3000.0f) + (int)(cy * 1700.0f);
				float dot_x = (float)cell_x + 0.2f + 0.6f * hash_unit(cell_x * 7 + key, cell_y);
				float dot_y = (float)cell_y + 0.2f + 0.6f * hash_unit(cell_y * 5, cell_x + key);
				float stretch = 1.0f + 1.5f * hash_unit(cell_x + key, cell_y * 13);
				float dot = sqrtf((gx - dot_x) * (gx - dot_x) * stretch + (gy - dot_y) * (gy - dot_y));
				float which = hash_unit(cell_x * 3 + key, cell_y * 3 + 11);

				speck = which < 0.5f ? clampf(1.0f - dot / 0.13f, 0.0f, 1.0f) * (1.0f - r * r) : 0.0f;
				blemish = which > 0.985f ? clampf(1.0f - dot / 0.22f, 0.0f, 1.0f) : 0.0f;
			}
			for (int k = 0; k < 3; k++)
			{
				float toward_stalk = clampf(-along / radius, 0.0f, 1.0f);

				color_here[k] = mixf(color[k], rim[k], smooth_step(0.35f, 1.0f, r));
				color_here[k] = mixf(color_here[k], rim[k] * 0.85f, toward_stalk * toward_stalk * 0.35f);
				color_here[k] *= 1.0f + (fractal_noise(side / radius * 3.0f + cx * 50.0f, along / radius * 3.0f, 3) - 0.5f) * 0.18f;
				// (a paler, pinker side, as it ripens unevenly)
				color_here[k] = mixf(color_here[k], pale[k], pale_share * clampf(1.0f - sqrtf((px - pale_x) * (px - pale_x)
					+ (py - pale_y) * (py - pale_y)) / (radius * 1.1f), 0.0f, 1.0f));
				// (marbled: paler pinker streaks and patches through the red,
				// drawn out round it a little)
				color_here[k] = mixf(color_here[k], pale[k] * 0.95f,
					smooth_step(0.38f, 0.8f, fractal_noise(side / radius * 9.0f + cx * 40.0f, along / radius * 4.5f + cy * 40.0f, 4)) * 0.4f);
				color_here[k] = mixf(color_here[k], pale[k] * 1.1f, speck * 0.5f);
				color_here[k] = mixf(color_here[k], calyx_dark[k] * 2.0f, blemish * 0.7f);
				// (the dimple where the stalk goes in: darker, a pale ring round it)
				color_here[k] = mixf(color_here[k], rim[k] * 0.6f, smooth_step(0.2f, 1.0f, dimple) * 0.6f);

				color_here[k] *= brightness;
				// (the crown: dark in its hollow, paler towards its points)
				if (crown > 0.0f)
				{
					float out = clampf(crown_distance / fmaxf(crown_radius, 1e-5f), 0.0f, 1.0f);
					float crown_color = mixf(calyx_dark[k], calyx_light[k], out * point);

					crown_color = mixf(crown_color, calyx_dark[k] * 0.5f, clampf(1.0f - crown_distance / (radius * 0.05f), 0.0f, 1.0f));
					color_here[k] = mixf(color_here[k], crown_color, crown);
				}
			}
			coverage = fmaxf(coverage, crown);
			pixel = &canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4];
			for (int k = 0; k < 3; k++)
			{
				pixel[k] = color_here[k] * coverage + pixel[k] * (1.0f - coverage);
			}
			pixel[3] = coverage + pixel[3] * (1.0f - coverage);
		}
	}
}

// A berry's skin for the plant grower's round fruit (fruit_shape =
// sphere): the picture wraps round it, its top at the stalk, its bottom at
// the foot. The whole picture skin: red, marbled paler and pinker, fine
// pale dots, now and then a blemish; darker towards the stalk (a dimple at
// the very top) and at the foot the dry flower's crown, five brown points
// in from the bottom edge; across, it tiles (the left edge meets the right)
static void draw_berry_skin(canvas_t *canvas, random_t *random)
{
	const float color[3] = { 0.86f, 0.22f, 0.22f };
	const float rim[3] = { 0.58f, 0.08f, 0.15f };
	const float pale[3] = { 0.96f, 0.50f, 0.48f };
	const float calyx_dark[3] = { 0.14f, 0.07f, 0.04f };
	const float calyx_light[3] = { 0.42f, 0.28f, 0.16f };
	float seed = random_range(random, 0.0f, 100.0f);
	float pale_u = random_float(random);
	float pale_share = random_range(random, 0.25f, 0.6f);
	float crown_turn = random_range(random, 0.0f, 1.0f);
	int width = canvas->width;
	int height = canvas->height;

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			float u = ((float)x + 0.5f) / (float)width;
			float v = ((float)y + 0.5f) / (float)height;
			// (round it: the noise on a circle, so it tiles across)
			float ring_x = cosf(u * 2.0f * PI_F);
			float ring_y = sinf(u * 2.0f * PI_F);
			float marble = fractal_noise(ring_x * 2.2f + seed, ring_y * 2.2f + v * 5.0f, 4);
			float broad = fractal_noise(ring_x * 0.9f + seed * 2.0f, ring_y * 0.9f + v * 1.6f, 3);
			float pale_here = pale_share * clampf(1.0f - fabsf(remainderf(u - pale_u, 1.0f)) / 0.35f, 0.0f, 1.0f)
				* clampf(1.0f - fabsf(v - 0.5f) / 0.45f, 0.0f, 1.0f);
			float point = powf(fabsf(cosf((u + crown_turn) * 5.0f * PI_F)), 3.0f);
			float crown_edge = 1.0f - (0.06f + 0.1f * point);
			float crown = smooth_step(crown_edge - 0.008f, crown_edge + 0.008f, v);
			float speck = 0.0f;
			float blemish = 0.0f;
			float *pixel = &canvas->pixels[((size_t)y * (size_t)width + (size_t)x) * 4];

			{
				// (dots in a grid that tiles across)
				const int cells_across = 28;
				float gx = u * (float)cells_across;
				float gy = v * (float)cells_across * 0.5f;
				int cell_x = (int)floorf(gx);
				int cell_y = (int)floorf(gy);
				int wrapped = cell_x % cells_across;
				float dot_x = (float)cell_x + 0.2f + 0.6f * hash_unit(wrapped * 7 + 3, cell_y);
				float dot_y = (float)cell_y + 0.2f + 0.6f * hash_unit(cell_y * 5, wrapped + 9);
				float dot = sqrtf((gx - dot_x) * (gx - dot_x) * 2.0f + (gy - dot_y) * (gy - dot_y));
				float which = hash_unit(wrapped * 3 + 1, cell_y * 3 + 11);

				speck = which < 0.5f ? clampf(1.0f - dot / 0.15f, 0.0f, 1.0f) : 0.0f;
				blemish = which > 0.985f ? clampf(1.0f - dot / 0.25f, 0.0f, 1.0f) : 0.0f;
			}
			for (int k = 0; k < 3; k++)
			{
				float value = mixf(color[k], rim[k], smooth_step(0.4f, 0.7f, broad) * 0.6f);

				value = mixf(value, rim[k] * 0.85f, smooth_step(0.35f, 0.0f, v) * 0.45f);
				value = mixf(value, rim[k], smooth_step(0.75f, 0.95f, v) * 0.3f);
				value *= 1.0f + (marble - 0.5f) * 0.2f;
				value = mixf(value, pale[k], pale_here);
				value = mixf(value, pale[k] * 0.95f, smooth_step(0.5f, 0.75f, marble) * 0.4f);
				value = mixf(value, pale[k] * 1.1f, speck * 0.5f);
				value = mixf(value, calyx_dark[k] * 2.0f, blemish * 0.7f);
				// (the dimple at the stalk; the very top row the stalk's
				// own brown: the grower colors the stalk from it)
				value = mixf(value, rim[k] * 0.6f, smooth_step(0.08f, 0.0f, v) * 0.7f);
				value = mixf(value, calyx_light[k] * 0.9f, smooth_step(0.035f, 0.02f, v));
				// (the crown)
				value = mixf(value, mixf(calyx_dark[k], calyx_light[k], point * smooth_step(crown_edge, 1.0f, v) * 0.9f), crown);
				pixel[k] = value;
			}
			pixel[3] = 1.0f;
		}
	}
}

// Noise round a band that tiles across: u round it (0 .. 1), down along
// it, in the same units as round (the band's height over its width times
// 2 pi, at most): three flat noises, each seen from one of three ways
// round the band, mixed by how much the point faces each (no seam, no
// stretch, and no one mirror showing)
static float wrapped_noise(float u, float down, float frequency, float seed, int octaves)
{
	float sum = 0.0f;
	float weights = 0.0f;

	for (int k = 0; k < 3; k++)
	{
		float angle = u * 2.0f * PI_F - (float)k * PI_F / 3.0f;
		float weight = cosf(angle) * cosf(angle);

		sum += weight * fractal_noise(sinf(angle) * frequency + seed + (float)k * 37.0f, down * frequency + (float)k * 11.0f, octaves);
		weights += weight;
	}
	return sum / weights;
}

// A blueberry's skin, as the berry skin above (the top at the stalk, the
// bottom at the foot, tiling across): dark blue-purple under a dusty pale
// bloom, patchy, rubbed off here and there in streaks (the dark showing);
// a small scar at the stalk, the very top row the stalk's own reddish
// brown; at the foot a sunken ring and the dry flower's crown of five
// upturned lobes, dusty at their edges, dark down inside
static void draw_blueberry_skin(canvas_t *canvas, random_t *random)
{
	const float under[3] = { 0.12f, 0.13f, 0.30f };
	const float bloom[3] = { 0.48f, 0.57f, 0.76f };
	const float deep[3] = { 0.08f, 0.06f, 0.12f };
	const float stalk[3] = { 0.42f, 0.27f, 0.20f };
	float seed = random_range(random, 0.0f, 100.0f);
	float crown_turn = random_range(random, 0.0f, 1.0f);
	float bloom_amount = random_range(random, 0.6f, 0.8f);
	int width = canvas->width;
	int height = canvas->height;

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			float u = ((float)x + 0.5f) / (float)width;
			float v = ((float)y + 0.5f) / (float)height;
			// (down it as far in the noise as round it: half round)
			float down = v * PI_F;
			float patches = wrapped_noise(u, down, 1.6f, seed, 4);
			float fine = wrapped_noise(u, down, 9.0f, seed * 2.0f, 3);
			// (rubbed: patches, from a touch)
			float rub = smooth_step(0.64f, 0.78f, wrapped_noise(u, down, 2.5f, seed * 3.0f, 3));
			float lobe = powf(fabsf(cosf((u + crown_turn) * 5.0f * PI_F)), 2.5f);
			float crown_edge = 1.0f - (0.06f + 0.06f * lobe);
			float bloom_here;
			float *pixel = &canvas->pixels[((size_t)y * (size_t)width + (size_t)x) * 4];

			bloom_here = bloom_amount * (0.8f + 0.7f * (patches - 0.5f) + 0.4f * (fine - 0.5f)) * (1.0f - rub * 0.55f);
			// (less on the crown, none down inside it)
			bloom_here *= 1.0f - smooth_step(crown_edge, crown_edge + 0.04f, v) * 0.85f;
			for (int k = 0; k < 3; k++)
			{
				float value = under[k] * (0.9f + 0.25f * (patches - 0.5f));

				value = mixf(value, bloom[k] * (0.92f + 0.16f * fine), clampf(bloom_here, 0.0f, 1.0f));
				// (round the stalk: a little darker, the scar a dark ring
				// with a paler middle, the top row the stalk's brown)
				value = mixf(value, under[k] * 0.8f, smooth_step(0.12f, 0.0f, v) * 0.5f);
				value = mixf(value, deep[k], clampf(1.0f - fabsf(v - 0.025f) / 0.012f, 0.0f, 1.0f) * 0.5f);
				value = mixf(value, stalk[k], smooth_step(0.016f, 0.008f, v));
				// (the sunken ring just outside the crown: darker)
				value = mixf(value, under[k] * 0.7f, clampf(1.0f - fabsf(v - (crown_edge - 0.03f)) / 0.035f, 0.0f, 1.0f) * 0.45f);
				// (the lobes: a dusty lip at their edge, then dark inside,
				// darkest in the middle)
				if (v > crown_edge)
				{
					float inside = (v - crown_edge) / (1.0f - crown_edge);

					value = mixf(value, bloom[k] * 0.85f, (1.0f - smooth_step(0.0f, 0.25f, inside)) * 0.6f);
					value = mixf(value, deep[k] * (0.8f + 0.4f * fine), smooth_step(0.15f, 0.6f, inside));
				}
				pixel[k] = value;
			}
			pixel[3] = 1.0f;
		}
	}
}

// A point on a quadratic curve, and its heading (radians, 0 up)
static void quadratic_at(float ax, float ay, float cx, float cy, float bx, float by, float t, float *x, float *y, float *heading)
{
	float u = 1.0f - t;

	*x = u * u * ax + 2.0f * u * t * cx + t * t * bx;
	*y = u * u * ay + 2.0f * u * t * cy + t * t * by;
	*heading = atan2f(2.0f * u * (cx - ax) + 2.0f * t * (bx - cx), -(2.0f * u * (cy - ay) + 2.0f * t * (by - cy)));
}

// A bud at (x, y) pointing at angle (radians, 0 up), length long: a
// pointed egg, a little bent and uneven (each its own), of three scales:
// two outer ones wrapping it from either side, their edges overlapping
// (a pale lip on the one over, a dark crease under it), a third peeping
// out at the tip. The stem's brown with a little red in it, darker at its
// foot, its tip and in the creases, finely streaked along; round across.
// Darkens what lies under it a little round its outline. No light baked in
static void draw_bud(canvas_t *canvas, float x0, float y0, float angle, float length, float shade)
{
	const float bud_color[3] = { 0.46f, 0.30f, 0.18f };
	const float bud_red[3] = { 0.52f, 0.24f, 0.14f };
	const float bud_dark[3] = { 0.22f, 0.13f, 0.08f };
	const float bud_lip[3] = { 0.64f, 0.48f, 0.32f };
	float s = canvas->scale;
	float reach = length * 1.4f * s + 2.0f;
	int left = (int)floorf(x0 * s - reach);
	int right = (int)ceilf(x0 * s + reach);
	int top = (int)floorf(y0 * s - reach);
	int bottom = (int)ceilf(y0 * s + reach);
	float along_x = sinf(angle);
	float along_y = -cosf(angle);
	float half_width = length * 0.36f;
	// (its own: how it bends, which side is fuller, where its scales meet)
	float seed = hash_unit((int)(x0 * 2311.0f), (int)(y0 * 1709.0f)) * 100.0f;
	float bend = (hash_unit((int)(y0 * 997.0f), (int)(x0 * 1453.0f)) - 0.5f) * 0.9f;
	float fuller = (hash_unit((int)(x0 * 613.0f), (int)(y0 * 389.0f)) - 0.5f) * 0.5f;
	float meet = (hash_unit((int)(y0 * 271.0f), (int)(x0 * 829.0f)) - 0.5f) * 0.4f;

	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right >= canvas->width ? canvas->width - 1 : right;
	bottom = bottom >= canvas->height ? canvas->height - 1 : bottom;
	for (int y = top; y <= bottom; y++)
	{
		for (int x = left; x <= right; x++)
		{
			float px = ((float)x + 0.5f) / s - x0;
			float py = ((float)y + 0.5f) / s - y0;
			float u = px * along_x + py * along_y;
			float v = px * -along_y + py * along_x;
			float t = u / length;
			float width;
			float coverage;
			float *pixel = &canvas->pixels[((size_t)y * (size_t)canvas->width + (size_t)x) * 4];

			if (t < -0.15f || t > 1.15f)
			{
				continue;
			}
			// (bent: more towards its tip; one side fuller; its outline a
			// little lumpy)
			v -= bend * length * t * t;
			width = t <= 0.0f || t >= 1.0f ? 0.0f : half_width * powf(sinf(PI_F * powf(t, 0.7f)), 0.75f) * (1.0f - 0.4f * t);
			width *= (v > 0.0f ? 1.0f + fuller : 1.0f - fuller) * (1.0f + (fractal_noise(t * 5.0f + seed, v > 0.0f ? 3.0f : -3.0f, 2) - 0.5f) * 0.4f);
			coverage = width > 0.0f ? clampf((width - fabsf(v)) * s + 0.5f, 0.0f, 1.0f) : 0.0f;
			if (t > -0.1f && t < 1.1f && fabsf(v) - width < half_width * 0.4f)
			{
				darken_under(pixel, fmaxf(fabsf(v) - width, 0.0f), half_width * 0.4f, 0.4f);
			}
			if (coverage <= 0.0f)
			{
				continue;
			}
			{
				float across = v / fmaxf(width, 1e-5f);
				// (round across, fuller a little below its middle)
				float round = 0.7f + 0.36f * sqrtf(fmaxf(1.0f - across * across, 0.0f)) + 0.06f * (1.0f - fabsf(t - 0.4f) * 2.0f);
				float streak = fractal_noise(across * 9.0f + seed, t * 2.0f, 2);
				float grain = value_noise((float)x / 1.5f, (float)y / 1.5f);
				// (its own blotches, scaled with it, and fine specks: darker
				// and paler dots)
				float blotch = fractal_noise(t * 7.0f + seed, across * 3.5f + seed, 3);
				float speck = fractal_noise((float)x / 2.2f + seed * 10.0f, (float)y / 2.2f, 2);
				// (the outer scale from the right over the left one: its
				// edge a curve leaning across, up to near the tip)
				float edge_a = across - (meet + 0.35f - 0.55f * t);
				// (the left one's own edge, low on the right: a round top)
				float edge_b = (t - (0.42f + 0.18f * across * across + meet * 0.2f)) / 0.4f;
				// (the inner scale out at the tip, between them)
				float inner = smooth_step(0.78f, 0.86f, t) * (1.0f - smooth_step(0.18f, 0.3f, fabsf(across - meet * 0.5f)));
				float lip = 0.0f;
				float crease = 0.0f;
				float dark;

				if (t > 0.1f && t < 0.92f)
				{
					// (over the edge: a pale lip; past it, under: a crease)
					lip += clampf(1.0f - fabsf(edge_a + 0.05f) / 0.05f, 0.0f, 1.0f) * 0.75f;
					crease += clampf(1.0f - fabsf(edge_a - 0.04f) / 0.06f, 0.0f, 1.0f);
				}
				if (across > 0.15f)
				{
					lip += clampf(1.0f - fabsf(edge_b + 0.06f) / 0.06f, 0.0f, 1.0f) * 0.45f * smooth_step(0.15f, 0.35f, across);
					crease += clampf(1.0f - fabsf(edge_b - 0.05f) / 0.07f, 0.0f, 1.0f) * 0.8f * smooth_step(0.15f, 0.35f, across);
				}
				dark = (1.0f - smooth_step(0.0f, 0.25f, t)) * 0.55f + smooth_step(0.8f, 1.0f, t) * 0.45f + crease * 0.85f + inner * 0.4f;
				for (int k = 0; k < 3; k++)
				{
					float color = mixf(bud_color[k], bud_red[k], smooth_step(0.4f, 0.9f, t) * 0.6f + (streak - 0.5f) * 0.5f);

					color = mixf(color, bud_lip[k], clampf(lip, 0.0f, 1.0f));
					color = mixf(color, bud_dark[k], clampf(dark, 0.0f, 1.0f));
					color *= round * (0.88f + 0.24f * streak) * (0.85f + 0.3f * grain) * (0.75f + 0.5f * blotch) * shade;
					color = mixf(color, bud_dark[k], smooth_step(0.68f, 0.78f, speck) * 0.6f);
					color = mixf(color, bud_lip[k], (1.0f - smooth_step(0.22f, 0.3f, speck)) * 0.5f);
					pixel[k] = color * coverage + pixel[k] * (1.0f - coverage);
				}
				pixel[3] = coverage + pixel[3] * (1.0f - coverage);
			}
		}
	}
}

// A stem's own marks along a quadratic curve from a to b over c, radius
// ra .. rb: small pale lenticels drawn out along it, fine dark streaks,
// now and then a pale fleck of old bark; drawn over the stem, under what
// comes after
static void stem_marks(canvas_t *canvas, random_t *random, float ax, float ay, float cx, float cy, float bx, float by, float ra, float rb,
	int count)
{
	paint_t lenticel = { .dark = { 0.58f, 0.52f, 0.40f }, .light = { 0.72f, 0.66f, 0.50f }, .tip = { 0.70f, 0.64f, 0.48f }, .tip_from = 1.0f };
	paint_t streak = { .dark = { 0.14f, 0.09f, 0.06f }, .light = { 0.22f, 0.15f, 0.10f }, .tip = { 0.20f, 0.14f, 0.09f }, .tip_from = 1.0f };

	for (int i = 0; i < count; i++)
	{
		float t = random_range(random, 0.03f, 0.97f);
		float u = 1.0f - t;
		float x = u * u * ax + 2.0f * u * t * cx + t * t * bx;
		float y = u * u * ay + 2.0f * u * t * cy + t * t * by;
		float dx = 2.0f * u * (cx - ax) + 2.0f * t * (bx - cx);
		float dy = 2.0f * u * (cy - ay) + 2.0f * t * (by - cy);
		float d = sqrtf(dx * dx + dy * dy);
		float radius = mixf(ra, rb, t);
		float off = random_range(random, -0.65f, 0.65f) * radius;
		float length = radius * random_range(random, 0.5f, 1.2f);
		bool dark = random_float(random) < 0.45f;

		if (d < 1e-6f)
		{
			continue;
		}
		dx /= d;
		dy /= d;
		x += -dy * off;
		y += dx * off;
		draw_segment(canvas, x - dx * length * 0.5f, y - dy * length * 0.5f, radius * (dark ? 0.08f : 0.14f), x + dx * length * 0.5f,
			y + dy * length * 0.5f, radius * (dark ? 0.06f : 0.1f), 0.0f, 1.0f, dark ? &streak : &lenticel);
	}
}

// A shoot's leaves: alternate, out each side in turn, the lower ones
// longer, those at the tip small, young and close; turned this way and
// that (some seen edge on). first_side: the first leaf's side (-1 left, 1
// right, 0 either); gives the side the next one would go
static float draw_shoot_leaves(canvas_t *canvas, random_t *random, float ax, float ay, float cx, float cy, float bx, float by, float ra,
	float rb, float from, int count, float length, float behind_chance, float first_side, const oval_leaf_paint_t *paint)
{
	float side = first_side != 0.0f ? first_side : random_float(random) < 0.5f ? -1.0f : 1.0f;

	// (with depths: the shoot's points kept, leaves kept off them)
	for (int i = 0; i <= 10 && behind_chance > 0.0f && wood_sample_count < WOOD_SAMPLES_SIZE; i++)
	{
		float heading;

		quadratic_at(ax, ay, cx, cy, bx, by, from + (1.0f - from) * (float)i / 10.0f, &wood_samples[wood_sample_count][0],
			&wood_samples[wood_sample_count][1], &heading);
		wood_sample_count++;
	}

	for (int i = 0; i < count; i++)
	{
		// (closer towards the tip: the last ones bunched round it)
		float f = ((float)i + random_range(random, -0.25f, 0.25f)) / (float)count;
		float t = from + (1.0f - from) * clampf(1.0f - (1.0f - f) * (1.0f - f) * 0.85f - 0.15f * (1.0f - f), 0.0f, 1.0f);
		float tip = clampf((t - 0.8f) / 0.2f, 0.0f, 1.0f);
		float x;
		float y;
		float heading;
		float out = random_range(random, 0.45f, 0.9f) * mixf(1.0f, 0.6f, tip);
		float leaf_length = length * random_range(random, 0.8f, 1.15f) * mixf(1.0f, 0.6f, tip * tip);
		// (now and then one turned over: its pale underside, seen edge on)
		bool over = random_float(random) < 0.2f;
		float seen = over ? random_range(random, 0.45f, 0.7f) : random_range(random, 0.78f, 1.0f);

		quadratic_at(ax, ay, cx, cy, bx, by, fminf(t, 1.0f), &x, &y, &heading);
		// (its stalk from the shoot's side, not from its middle: ra .. rb
		// the shoot's radius)
		{
			float stem_radius = mixf(ra, rb, fminf(t, 1.0f)) * 0.85f;

			x += sinf(heading + side * out) * stem_radius;
			y -= cosf(heading + side * out) * stem_radius;
		}
		queue_oval_leaf(canvas, x, y, heading + side * out, leaf_length, leaf_length * random_range(random, 0.26f, 0.31f),
			-side * random_range(random, -0.15f, 0.45f), seen, tip * 0.8f, random_range(random, 0.85f, 1.18f),
			over ? random_range(random, 0.5f, 0.8f) : 0.0f, paint,
			behind_chance > 0.0f ? (random_float(random) < behind_chance ? -1.0f : 1.0f) * random_range(random, 0.15f, 1.0f) : 0.0f);
		// (with depths: a little bud in its axil, between its stalk and
		// the shoot, leaning out with it)
		if (behind_chance > 0.0f && leaf_queue_count > 0)
		{
			leaf_queue[leaf_queue_count - 1].bud_angle = heading + side * out * 0.4f;
			leaf_queue[leaf_queue_count - 1].bud_length = length * random_range(random, 0.05f, 0.07f) * mixf(1.0f, 0.6f, tip);
		}
		side = -side;
	}
	return side;
}

static void draw_cranberry(canvas_t *canvas, random_t *random, bool with_berries)
{
	// (lingonberry's: dark glossy green, a pale midrib, the rim rolled
	// under darker; the young leaves lighter, yellower)
	oval_leaf_paint_t leaf = {
		.blade = { 0.13f, 0.29f, 0.11f }, .midrib = { 0.34f, 0.46f, 0.17f }, .rim = { 0.08f, 0.19f, 0.07f },
		.young = { 0.36f, 0.52f, 0.18f }, .under = { 0.46f, 0.56f, 0.30f }, .mottle = 0.35f,
	};
	paint_t bark = { .dark = { 0.24f, 0.17f, 0.11f }, .light = { 0.50f, 0.38f, 0.26f }, .tip = { 0.42f, 0.28f, 0.14f }, .tip_from = 0.6f,
		.rough = 0.9f, .bumps = 0.6f };
	paint_t young_stem = { .dark = { 0.26f, 0.15f, 0.09f }, .light = { 0.52f, 0.36f, 0.22f }, .tip = { 0.40f, 0.42f, 0.16f }, .tip_from = 0.75f,
		.rough = 0.7f, .bumps = 0.5f };
	paint_t stalk = { .dark = { 0.24f, 0.11f, 0.07f }, .light = { 0.44f, 0.24f, 0.15f }, .tip = { 0.40f, 0.18f, 0.12f }, .tip_from = 0.8f,
		.rough = 0.4f };
	float berry_color[3] = { 0.86f, 0.22f, 0.22f };
	float berry_rim[3] = { 0.58f, 0.08f, 0.15f };
	float lean = random_range(random, -0.08f, 0.08f);
	// The stem: from the foot up, woody below, the young shoot above
	float ax = 0.5f;
	float ay = canvas->tall - 0.03f;
	float bx = 0.5f + lean * 2.0f + random_range(random, -0.05f, 0.05f);
	float by = 0.18f;
	float cx = 0.5f + random_range(random, -0.12f, 0.12f);
	float cy = (ay + by) * 0.5f;
	// (the leaves-only twig, on its plant's own branches: leafy from its foot)
	float wood_t = with_berries ? random_range(random, 0.2f, 0.26f) : random_range(random, 0.03f, 0.05f);
	float mx;
	float my;
	float heading;
	float side = random_float(random) < 0.5f ? -1.0f : 1.0f;

	quadratic_at(ax, ay, cx, cy, bx, by, wood_t, &mx, &my, &heading);
	// (the old wood in a few crooked pieces, a knob at each joint: last
	// years' growth)
	{
		const int pieces = 2;
		float last_x = ax;
		float last_y = ay;

		for (int p = 1; p <= pieces; p++)
		{
			float f = (float)p / (float)pieces;
			float x = mixf(ax, mx, f) + (p < pieces ? random_range(random, -0.035f, 0.035f) : 0.0f);
			float y = mixf(ay, my, f);
			float r0 = mixf(0.034f, 0.026f, (float)(p - 1) / (float)pieces);
			float r1 = mixf(0.034f, 0.026f, f);

			draw_curve(canvas, last_x, last_y, (last_x + x) * 0.5f + random_range(random, -0.012f, 0.012f), (last_y + y) * 0.5f, x, y, r0, r1, &bark);
			if (p < pieces)
			{
				draw_segment(canvas, x, y - 0.012f, r1 * 1.35f, x, y + 0.012f, r1 * 1.35f, 0.0f, 1.0f, &bark);
				// (a stub of a shoot that fell, now and then)
				if (random_float(random) < 0.5f)
				{
					float stub_side = random_float(random) < 0.5f ? -1.0f : 1.0f;

					draw_segment(canvas, x, y, r1 * 0.8f, x + stub_side * 0.035f, y - 0.025f, r1 * 0.5f, 0.0f, 1.0f, &bark);
				}
			}
			last_x = x;
			last_y = y;
		}
	}
	// The young stem, bowed (its bow where its leaves go too)
	float young_cx = (mx + bx) * 0.5f + (cx - 0.5f) * 0.4f + random_range(random, -0.07f, 0.07f);
	float young_cy = (my + by) * 0.5f;

	draw_curve(canvas, mx, my, young_cx, young_cy, bx, by, 0.025f, 0.011f, &young_stem);
	// A side shoot out of the young part, the other way from the berries,
	// and now and then a second, higher, on the berries' side; each bowed
	// its own way. Their wood first, then their leaves over all of it
	{
		float shoot_points[2][6];
		int shoot_leaves[2];
		float shoot_leaf_length[2];
		int shoot_count = random_float(random) < 0.7f ? 2 : 1;

		for (int k = 0; k < shoot_count; k++)
		{
			float t = k == 0 ? random_range(random, 0.15f, 0.3f) : random_range(random, 0.5f, 0.62f);
			float out = (k == 0 ? side : -side) * (k == 0 ? random_range(random, 0.55f, 0.75f) : random_range(random, 0.5f, 0.7f));
			float shoot_length = k == 0 ? random_range(random, 0.38f, 0.48f) : random_range(random, 0.26f, 0.34f);
			float bow = random_range(random, -0.3f, 0.3f) * shoot_length;
			float sx;
			float sy;
			float *point = shoot_points[k];

			quadratic_at(mx, my, young_cx, young_cy, bx, by, t, &sx, &sy, &heading);
			point[0] = sx;
			point[1] = sy;
			point[4] = sx + sinf(heading + out) * shoot_length;
			point[5] = sy - cosf(heading + out) * shoot_length;
			point[2] = sx + sinf(heading + out * 1.3f) * shoot_length * 0.5f + cosf(heading + out) * bow;
			point[3] = sy - cosf(heading + out * 1.3f) * shoot_length * 0.5f + sinf(heading + out) * bow;
			shoot_leaves[k] = k == 0 ? 10 : 7;
			shoot_leaf_length[k] = k == 0 ? 0.2f : 0.18f;
			draw_curve(canvas, point[0], point[1], point[2], point[3], point[4], point[5], 0.014f, 0.0075f, &young_stem);
		}
		for (int k = 0; k < shoot_count; k++)
		{
			const float *point = shoot_points[k];

			draw_shoot_leaves(canvas, random, point[0], point[1], point[2], point[3], point[4], point[5], 0.014f, 0.0075f, 0.12f, shoot_leaves[k], 0.0f, 0.0f,
				shoot_leaf_length[k], &leaf);
		}
	}
	// The stem's leaves, on its young part
	draw_shoot_leaves(canvas, random, mx, my, young_cx, young_cy, bx, by, 0.025f, 0.011f, 0.02f, 19, 0.24f, 0.0f, 0.0f, &leaf);
	flush_oval_leaves(canvas, NULL);
	// The berries: two or three hanging off a short stalk out of the stem,
	// on the side away from the shoot
	if (random_float(random) < 0.85f && with_berries)
	{
		// (anywhere from low on the young shoot to near its tip)
		float t = random_range(random, 0.08f, 0.72f);
		float nx;
		float ny;
		int berries = 2 + (random_float(random) < 0.5f ? 1 : 0);
		float out = -side;
		float base_x;
		float base_y;

		quadratic_at(mx, my, young_cx, young_cy, bx, by, t, &nx, &ny, &heading);
		base_x = nx + out * 0.1f;
		base_y = ny + 0.02f;
		draw_curve(canvas, nx, ny, nx + out * 0.06f, ny - 0.015f, base_x, base_y, 0.008f, 0.006f, &stalk);
		// (each its own way down off the stalk's end, bunched: the first
		// out, the next under it and in, the third further out; the
		// stalks first, the berries over them)
		float berry_x[3];
		float berry_y[3];
		float berry_radius[3];
		float berry_tint[3];

		for (int b = 0; b < berries; b++)
		{
			berry_radius[b] = random_range(random, 0.1f, 0.12f);
			berry_x[b] = base_x + out * (0.04f + (b == 1 ? 0.03f : (float)b * 0.05f)) + random_range(random, -0.015f, 0.015f);
			berry_y[b] = base_y + 0.1f + (float)b * 0.12f * (b == 2 ? 0.55f : 1.0f) + random_range(random, -0.01f, 0.01f);
			berry_tint[b] = random_range(random, -0.25f, 0.15f);
			{
				float dx = berry_x[b] - base_x;
				float dy = berry_y[b] - base_y;
				float d = sqrtf(dx * dx + dy * dy);

				draw_curve(canvas, base_x, base_y, base_x + dx * 0.4f, base_y + dy * 0.3f, berry_x[b] - dx / d * berry_radius[b] * 0.9f,
					berry_y[b] - dy / d * berry_radius[b] * 0.9f, 0.0055f, 0.0045f, &stalk);
			}
		}
		for (int b = berries - 1; b >= 0; b--)
		{
			float dx = berry_x[b] - base_x;
			float dy = berry_y[b] - base_y;
			float d = sqrtf(dx * dx + dy * dy);
			float color_here[3] = { berry_color[0] * (1.0f + berry_tint[b]), berry_color[1] * (1.0f + 2.0f * berry_tint[b]), berry_color[2] };

			draw_berry(canvas, berry_x[b], berry_y[b], berry_radius[b], dx / d, dy / d, color_here, berry_rim, random_range(random, 0.88f, 1.08f));
		}
	}
}

// A blueberry's leafy twig (leaves only: the fruit the grower's round
// ones): a brownish green stem, red brown buds at its joints, side shoots
// out of it alternately, the lower longer; elliptic leaves drawn to a
// point, fresh mid green, their veins showing, alternate along each shoot
static void draw_blueberry_leaves(canvas_t *canvas, random_t *random)
{
	oval_leaf_paint_t leaf = {
		.blade = { 0.22f, 0.40f, 0.18f }, .midrib = { 0.68f, 0.76f, 0.48f }, .rim = { 0.15f, 0.30f, 0.12f },
		.young = { 0.34f, 0.50f, 0.18f }, .under = { 0.44f, 0.54f, 0.32f }, .mottle = 0.3f, .pointed = 1.0f, .veins = 0.4f,
		.widest = 0.5f, .slender = 0.88f, .smooth = 1.0f, .matte = 1.0f, .stalk = 0.009f,
		.net = 1.0f,
	};
	paint_t stem = { .dark = { 0.24f, 0.17f, 0.10f }, .light = { 0.50f, 0.40f, 0.24f }, .tip = { 0.36f, 0.42f, 0.18f }, .tip_from = 0.55f,
		.rough = 0.7f, .bumps = 0.6f };
	paint_t young_stem = { .dark = { 0.24f, 0.28f, 0.11f }, .light = { 0.42f, 0.50f, 0.22f }, .tip = { 0.40f, 0.52f, 0.20f }, .tip_from = 0.3f,
		.rough = 0.3f, .bumps = 0.3f };
	paint_t shoot_stem = { .dark = { 0.26f, 0.22f, 0.10f }, .light = { 0.48f, 0.46f, 0.22f }, .tip = { 0.36f, 0.44f, 0.16f }, .tip_from = 0.4f,
		.rough = 0.5f, .bumps = 0.4f };
	float ax = 0.5f;
	float ay = canvas->tall - 0.03f;
	float bx = 0.5f + random_range(random, -0.12f, 0.12f);
	float by = 0.36f;
	float cx = 0.5f + random_range(random, -0.18f, 0.18f);
	float cy = (ay + by) * 0.5f;
	int shoots = 3;
	float side = random_float(random) < 0.5f ? -1.0f : 1.0f;
	float heading;
	// (the wood drawn on its own layer: some leaves go behind it)
	canvas_t wood = *canvas;

	wood.pixels = calloc((size_t)canvas->width * (size_t)canvas->height * 4, sizeof(float));
	if (wood.pixels == NULL)
	{
		return;
	}

	// The side shoots, alternately, the lower longer, out and up; a bud at
	// each joint. All the wood first, then the leaves over it (a shoot
	// drawn after another's leaves crossed over them)
	float shoot_points[4][6];
	int leaf_counts[4];

	for (int i = 0; i < shoots; i++)
	{
		float t = 0.07f + 0.55f * ((float)i + random_range(random, 0.0f, 0.4f)) / (float)shoots;
		float sx;
		float sy;
		float out = side * random_range(random, 0.55f, 0.8f);
		float shoot_length = random_range(random, 0.38f, 0.48f) * (1.0f - 0.45f * t);
		float bow = random_range(random, -0.3f, 0.3f) * shoot_length;
		float *point = shoot_points[i];

		quadratic_at(ax, ay, cx, cy, bx, by, t, &sx, &sy, &heading);
		// (the stem bent at the node, away from the shoot)
		sx -= side * random_range(random, 0.015f, 0.035f);
		point[0] = sx;
		point[1] = sy;
		point[2] = sx + sinf(heading + out * 1.25f) * shoot_length * 0.5f + cosf(heading + out) * bow;
		point[3] = sy - cosf(heading + out * 1.25f) * shoot_length * 0.5f + sinf(heading + out) * bow;
		point[4] = sx + sinf(heading + out) * shoot_length;
		point[5] = sy - cosf(heading + out) * shoot_length;
		leaf_counts[i] = 3 + (int)(random_float(random) * 2.99f);
		draw_curve(&wood, point[0], point[1], point[2], point[3], point[4], point[5], 0.014f, 0.006f, &shoot_stem);
		stem_marks(&wood, random, point[0], point[1], point[2], point[3], point[4], point[5], 0.014f, 0.006f, 10);
		// (a bud in the shoot's angle, another at its tip)
		draw_bud(&wood, sx + sinf(heading + out * 0.45f) * 0.012f, sy - cosf(heading + out * 0.45f) * 0.012f, heading + out * 0.45f,
			random_range(random, 0.06f, 0.075f), random_range(random, 0.85f, 1.1f));
		draw_bud(&wood, point[4], point[5], heading + out * 0.8f, random_range(random, 0.05f, 0.065f), random_range(random, 0.85f, 1.1f));
		side = -side;
	}
	// The stem: from node to node, bent a little at each, away from the
	// shoot out of it (drawn now: the leaves still to come over it); its
	// last piece kept, its own leaves go along it
	float top_piece[11];

	{
		float last_x = ax;
		float last_y = ay;

		for (int i = 0; i <= shoots; i++)
		{
			float x = i < shoots ? shoot_points[i][0] : bx;
			float y = i < shoots ? shoot_points[i][1] : by;
			float r0 = mixf(0.028f, 0.01f, (ay - last_y) / (ay - by));
			float r1 = mixf(0.028f, 0.01f, (ay - y) / (ay - by));

			float mid_x = (last_x + x) * 0.5f + random_range(random, -0.02f, 0.02f);

			if (i == shoots)
			{
				// (the last piece: a twist in its middle, a node there, and
				// greener past it, young)
				float twist_x = (last_x + x) * 0.5f + side * random_range(random, 0.04f, 0.06f);
				float twist_y = (last_y + y) * 0.5f;
				float r_mid = (r0 + r1) * 0.5f;

				draw_curve(&wood, last_x, last_y, (last_x + twist_x) * 0.5f - side * 0.01f, (last_y + twist_y) * 0.5f, twist_x, twist_y, r0, r_mid,
					&stem);
				stem_marks(&wood, random, last_x, last_y, (last_x + twist_x) * 0.5f, (last_y + twist_y) * 0.5f, twist_x, twist_y, r0, r_mid, 7);
				draw_curve(&wood, twist_x, twist_y, (twist_x + x) * 0.5f + side * 0.012f, (twist_y + y) * 0.5f, x, y, r_mid, r1, &young_stem);
				draw_segment(&wood, twist_x, twist_y + r_mid * 0.5f, r_mid * 1.15f, twist_x, twist_y - r_mid * 0.5f, r_mid * 1.1f, 0.4f, 0.6f,
					&stem);
				top_piece[0] = last_x;
				top_piece[1] = last_y;
				top_piece[2] = twist_x;
				top_piece[3] = twist_y;
				top_piece[4] = x;
				top_piece[5] = y;
				top_piece[6] = r0;
				top_piece[7] = r_mid;
				top_piece[8] = (twist_x + x) * 0.5f + side * 0.012f;
				top_piece[9] = (twist_y + y) * 0.5f;
				top_piece[10] = r1;
				break;
			}
			draw_curve(&wood, last_x, last_y, mid_x, (last_y + y) * 0.5f, x, y, r0, r1, &stem);
			stem_marks(&wood, random, last_x, last_y, mid_x, (last_y + y) * 0.5f, x, y, r0, r1, 14);
			// (the node: a little swollen, a leaf's scar under it)
			if (i < shoots)
			{
				draw_segment(&wood, x, y + r1 * 0.6f, r1 * 1.25f, x, y - r1 * 0.6f, r1 * 1.15f, 0.4f, 0.6f, &stem);
			}
			last_x = x;
			last_y = y;
		}
	}
	draw_bud(&wood, bx, by, 0.0f, 0.065f, 1.0f);
	for (int i = 0; i < shoots; i++)
	{
		const float *point = shoot_points[i];

		draw_shoot_leaves(canvas, random, point[0], point[1], point[2], point[3], point[4], point[5], 0.014f, 0.006f, 0.2f, leaf_counts[i], 0.36f,
			0.4f, 0.0f, &leaf);
	}
	// The stem's own leaves, along its last piece (past its last shoot)
	// (each half of it, the twist between them)
	// (one side then the other, on across the twist: no two at one place
	// on one side)
	{
		float next_side = draw_shoot_leaves(canvas, random, top_piece[0], top_piece[1], (top_piece[0] + top_piece[2]) * 0.5f,
			(top_piece[1] + top_piece[3]) * 0.5f, top_piece[2], top_piece[3], top_piece[6], top_piece[7], 0.15f, 2, 0.4f, 0.4f, 0.0f, &leaf);

		draw_shoot_leaves(canvas, random, top_piece[2], top_piece[3], top_piece[8], top_piece[9], top_piece[4], top_piece[5], top_piece[7],
			top_piece[10], 0.3f, 4, 0.36f, 0.4f, next_side, &leaf);
	}
	flush_oval_leaves(canvas, &wood);
	free(wood.pixels);
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
// aspect > 0: the picture cut to the twig (a few pixels round it) and
// widened (clear at its sides) or heightened (clear over it) to aspect
// times as tall as wide, the twig's foot at its bottom, in its middle
// across: the atlas's cells keep a picture's shape, so one the cell's
// shape fills it
static bool finish(const canvas_t *canvas, int width, int height, const char *path, float aspect)
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
	if (aspect > 0.0f)
	{
		int left = width;
		int right = -1;
		int bottom = -1;
		int new_width;
		int new_height;
		int offset_x;
		int offset_y;
		uint8_t *shaped;

		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				if (out[((size_t)y * (size_t)width + (size_t)x) * 4 + 3] != 0)
				{
					left = x < left ? x : left;
					right = x > right ? x : right;
					bottom = y > bottom ? y : bottom;
				}
			}
		}
		if (right >= left)
		{
			// (the twig with its foot's column in the middle: as far out
			// on both sides as its farther side)
			int middle = width / 2;
			int half = (middle - left > right - middle ? middle - left : right - middle) + 4;
			int content_height = bottom + 5 < height ? bottom + 5 : height;

			new_width = 2 * half;
			// (some clear over it, as a photographed twig has: the card is
			// as long as the picture, so a twig to its top edge comes out
			// longer than one with room over it)
			new_height = (int)lroundf((float)content_height * 1.12f);
			if ((float)new_height < aspect * (float)new_width)
			{
				new_height = (int)lroundf(aspect * (float)new_width);
			}
			else
			{
				new_width = (int)lroundf((float)new_height / aspect);
				half = new_width / 2;
			}
			shaped = calloc((size_t)new_width * (size_t)new_height * 4, 1);
			if (shaped != NULL)
			{
				offset_x = half - middle;
				offset_y = new_height - content_height;
				for (int y = 0; y < content_height; y++)
				{
					for (int x = 0; x < width; x++)
					{
						int to_x = x + offset_x;

						if (to_x < 0 || to_x >= new_width)
						{
							continue;
						}
						memcpy(&shaped[((size_t)(y + offset_y) * (size_t)new_width + (size_t)to_x) * 4], &out[((size_t)y * (size_t)width + (size_t)x) * 4], 4);
					}
				}
				free(out);
				out = shaped;
				width = new_width;
				height = new_height;
			}
		}
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
		fprintf(stderr, "Usage: potop_twig [--width W] [--seed N] pine|pine_brush|pine_rosette|pine_needles|spruce|juniper|cranberry|cranberry_leaves|blueberry_leaves|berry_skin|blueberry_skin|fern|fern_lobed OUT.png\n");
		return 1;
	}
	// (as tall as the kind's twig is: the pine's branch tip square,
	// the juniper's twig twice as tall as wide)
	canvas.tall = strcmp(kind, "pine") == 0 ? 1.75f : (strcmp(kind, "pine_needles") == 0 ? 4.5f : (strcmp(kind, "berry_skin") == 0 || strcmp(kind, "blueberry_skin") == 0 ? 0.5f : 2.0f));
	canvas.width = width * SUPERSAMPLE;
	canvas.height = (int)lroundf((float)width * canvas.tall) * SUPERSAMPLE;
	canvas.scale = (float)canvas.width;
	canvas.fit = 1.0f;
	canvas.foot_x = 0.5f;
	canvas.foot_y = canvas.tall;
	canvas.pixels = calloc((size_t)canvas.width * (size_t)canvas.height * 4, sizeof(float));
	canvas.depth = NULL;
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
	else if (strcmp(kind, "spruce") == 0)
	{
		draw_spruce(&canvas, &random);
	}
	else if (strcmp(kind, "cranberry") == 0 || strcmp(kind, "cranberry_leaves") == 0)
	{
		draw_cranberry(&canvas, &random, strcmp(kind, "cranberry") == 0);
	}
	else if (strcmp(kind, "blueberry_leaves") == 0)
	{
		draw_blueberry_leaves(&canvas, &random);
	}
	else if (strcmp(kind, "berry_skin") == 0)
	{
		draw_berry_skin(&canvas, &random);
	}
	else if (strcmp(kind, "blueberry_skin") == 0)
	{
		draw_blueberry_skin(&canvas, &random);
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
		fprintf(stderr, "potop_twig: no kind '%s' (pine, pine_brush, pine_rosette, pine_needles, spruce, juniper, cranberry, cranberry_leaves, blueberry_leaves, berry_skin, blueberry_skin, fern, fern_lobed)\n", kind);
		free(canvas.pixels);
		return 1;
	}
	if (!finish(&canvas, width, canvas.height / SUPERSAMPLE, path, strcmp(kind, "spruce") == 0 || strcmp(kind, "cranberry") == 0 || strcmp(kind, "cranberry_leaves") == 0 || strcmp(kind, "blueberry_leaves") == 0 ? 2.0f : 0.0f))
	{
		fprintf(stderr, "potop_twig: could not write %s\n", path);
		free(canvas.pixels);
		return 1;
	}
	free(canvas.pixels);
	printf("potop_twig: %s (%s, seed %u, %d wide)\n", path, kind, seed, width);
	return 0;
}
