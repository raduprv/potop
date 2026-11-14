// The houses' stand-in textures (house_stand_in_textures in house_grow.h):
// plaster, timber, roof tiles, the base's stone and a wall's stone, made
// from nothing, tiling by construction (every noise in them repeats over
// the picture), each as a color, a normal map (OpenGL's, green up) and an
// ARM map (occlusion, roughness, metal) derived from a height field. Stand
// ins: good enough to see the house by, to be replaced by painted ones.

#include "house_grow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "stb_image_write.h"

#define TEXTURE_SIZE 1024

static float clamp_float(float value, float low, float high)
{
	return value < low ? low : (value > high ? high : value);
}

static float smooth_step(float low, float high, float value)
{
	float t = clamp_float((value - low) / (high - low), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

static float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

static float fade_curve(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static uint32_t hash_cell(int x, int y, int z, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2246822519u + seed * 3266489917u;

	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return h;
}

static float hash_unit(int x, int y, int z, uint32_t seed)
{
	return (float)(hash_cell(x, y, z, seed) & 0xFFFFu) / 65535.0f;
}

// Value noise that repeats every period_x cells across and period_y down,
// x and y in cells
static float tiling_noise_xy(float x, float y, int period_x, int period_y, uint32_t seed)
{
	float fx = floorf(x);
	float fy = floorf(y);
	int ix = (int)fx;
	int iy = (int)fy;
	float u = fade_curve(x - fx);
	float v = fade_curve(y - fy);
	float c[4];

	for (int k = 0; k < 4; k++)
	{
		int cx = ((ix + (k & 1)) % period_x + period_x) % period_x;
		int cy = ((iy + (k >> 1)) % period_y + period_y) % period_y;

		c[k] = hash_unit(cx, cy, 0, seed);
	}
	return lerp(lerp(c[0], c[1], u), lerp(c[2], c[3], u), v);
}

// The same, as often across as down
static float tiling_noise(float x, float y, int period, uint32_t seed)
{
	return tiling_noise_xy(x, y, period, period, seed);
}

// 0..1, x and y 0..1 across the texture (it tiles); cells_x and cells_y
// the first octave's (stretched noise with different counts)
static float tiling_fractal(float x, float y, int cells_x, int cells_y, int octaves, uint32_t seed)
{
	float sum = 0.0f;
	float amplitude = 0.5f;
	float total = 0.0f;

	for (int o = 0; o < octaves; o++)
	{
		// (each way its own period: it repeats over the picture both ways,
		// stretched as asked; one period for both left a seam at the
		// picture's edge across the smaller count)
		sum += amplitude * tiling_noise_xy(x * (float)cells_x, y * (float)cells_y, cells_x, cells_y, seed + (uint32_t)o * 31u);
		total += amplitude;
		cells_x *= 2;
		cells_y *= 2;
		amplitude *= 0.5f;
	}
	return sum / total;
}

// Cracks: the edges between Voronoi cells (that tile), 0 on one .. 1 away
static float tiling_cracks(float x, float y, int cells, uint32_t seed)
{
	float px = x * (float)cells;
	float py = y * (float)cells;
	int ix = (int)floorf(px);
	int iy = (int)floorf(py);
	float nearest = 9.0f;
	float second = 9.0f;

	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int cx = ix + dx;
			int cy = iy + dy;
			uint32_t h = hash_cell(((cx % cells) + cells) % cells, ((cy % cells) + cells) % cells, 5, seed);
			float sx = (float)cx + (float)(h & 0xFFu) / 255.0f;
			float sy = (float)cy + (float)((h >> 8) & 0xFFu) / 255.0f;
			float distance = hypotf(px - sx, py - sy);

			if (distance < nearest)
			{
				second = nearest;
				nearest = distance;
			}
			else if (distance < second)
			{
				second = distance;
			}
		}
	}
	return clamp_float((second - nearest) * 6.0f, 0.0f, 1.0f);
}

static void mix_color(float out[3], const float a[3], const float b[3], float t)
{
	for (int k = 0; k < 3; k++)
	{
		out[k] = lerp(a[k], b[k], t);
	}
}

static void set_color(float out[3], float r, float g, float b)
{
	out[0] = r;
	out[1] = g;
	out[2] = b;
}

// ---------------------------------------------------------------------------
// The materials: color and height at a point (x, y in 0..1), and their
// roughness; height 0..1

typedef struct
{
	const char *name;
	void (*texel)(float x, float y, float color[3], float *height_out, float *roughness_out);
	float bumps;                           // how steep the normals come out of the heights
} stand_in_t;

// Lime plaster laid by hand: the trowel's low waves, the sand in it, its
// tone wandering a little from one batch to the next, hairline cracks in
// places, a patch here and there where the lime has scaled off to the
// coarse render under it. Clean otherwise: the rain's streaks, the dirt
// at the foot and the moss are the building shader's (by the weather and
// where on the house)
static void plaster_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	float broad = tiling_fractal(x, y, 3, 3, 3, 11u);
	float trowel = tiling_fractal(x, y, 9, 6, 3, 17u);
	float sand = tiling_fractal(x, y, 256, 256, 2, 12u);
	float coarse = tiling_fractal(x, y, 96, 96, 3, 19u);
	float tone = tiling_fractal(x, y, 5, 4, 3, 18u);
	float cracks = tiling_cracks(x, y, 4, 13u);
	float crack_line = 1.0f - smooth_step(0.0f, 0.022f, cracks);
	float crack_where = smooth_step(0.6f, 0.75f, tiling_fractal(x, y, 3, 3, 2, 14u));
	float scaling = tiling_fractal(x, y, 6, 6, 4, 16u);
	float scaled = smooth_step(0.83f, 0.86f, scaling);
	float rim = smooth_step(0.79f, 0.83f, scaling) * (1.0f - scaled);
	float lime[3];
	float aged[3];
	float render[3];
	float height;

	set_color(lime, 0.85f, 0.82f, 0.75f);
	set_color(aged, 0.77f, 0.73f, 0.65f);
	set_color(render, 0.62f, 0.58f, 0.52f);
	mix_color(color, lime, aged, tone * 0.55f + (1.0f - broad) * 0.2f);
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.95f + sand * 0.08f + (trowel - 0.5f) * 0.05f;
	}
	// Scaled off: the render under it, coarse, the lime's broken edge round it
	{
		float under[3];

		mix_color(under, render, aged, coarse * 0.3f);
		for (int k = 0; k < 3; k++)
		{
			under[k] *= 0.85f + coarse * 0.25f;
		}
		mix_color(color, color, under, scaled);
		for (int k = 0; k < 3; k++)
		{
			color[k] *= 1.0f - rim * 0.12f;
		}
	}
	height = 0.55f + (broad - 0.5f) * 0.25f + (trowel - 0.5f) * 0.22f + (sand - 0.5f) * 0.07f;
	height = height * (1.0f - scaled) + (0.3f + (coarse - 0.5f) * 0.2f) * scaled + rim * 0.05f;
	height -= crack_line * crack_where * 0.22f;
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 1.0f - crack_line * crack_where * 0.45f;
	}
	*height_out = clamp_float(height, 0.0f, 1.0f);
	*roughness_out = 0.9f + scaled * 0.06f;
}

// Hewn oak, centuries old: the grain in long lines along U, the adze's
// shallow scoops across it one strike after the other, checks (the long
// cracks of seasoning) along the grain, a few knots, the wood gone silver
// grey in patches where the weather took the brown out
static void timber_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	float grain = tiling_fractal(x, y, 2, 40, 4, 23u);
	float fibre = tiling_fractal(x, y, 4, 300, 2, 24u);
	float weather = tiling_fractal(x, y, 3, 8, 3, 27u);
	// (the grain's lines: the noise's ridges, sharp)
	float lines = 1.0f - fabsf(tiling_fractal(x, y, 3, 70, 3, 31u) - 0.5f) * 2.0f;
	float tone = tiling_fractal(x, y, 2, 6, 2, 29u);
	// The adze: a scoop every few centimeters along the grain, the strikes
	// of one pass in a band across, the next band's a little off
	float band = floorf(y * 9.0f);
	float strike = x * 28.0f + hash_unit((int)band, 0, 0, 28u) * 3.0f + (tiling_fractal(x, y, 4, 9, 2, 30u) - 0.5f) * 1.2f;
	float within = strike - floorf(strike);
	float scoop = (within - 0.5f) * (within - 0.5f) * 4.0f;
	// The checks: a few lanes across, a crack along each where the noise lets it
	float lane = y * 7.0f + (tiling_fractal(x, y, 3, 7, 2, 25u) - 0.5f) * 0.8f;
	float from_lane = fabsf(lane - floorf(lane) - 0.5f);
	float check = (1.0f - smooth_step(0.004f, 0.016f, from_lane)) * smooth_step(0.55f, 0.68f, tiling_fractal(x, y, 3, 7, 2, 26u));
	float brown[3];
	float dark[3];
	float grey[3];
	float height;

	set_color(brown, 0.41f, 0.31f, 0.22f);
	set_color(dark, 0.24f, 0.17f, 0.11f);
	set_color(grey, 0.5f, 0.47f, 0.42f);
	mix_color(color, brown, dark, grain * 0.35f + fibre * 0.15f + (tone - 0.5f) * 0.2f + smooth_step(0.75f, 0.95f, lines) * 0.45f);
	mix_color(color, color, grey, smooth_step(0.4f, 0.8f, weather) * 0.4f);
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.95f + scoop * 0.04f + (fibre - 0.5f) * 0.12f;
	}
	// Two knots, where the grain bends round them
	for (int knot = 0; knot < 2; knot++)
	{
		float kx = knot == 0 ? 0.27f : 0.71f;
		float ky = knot == 0 ? 0.33f : 0.68f;
		float dx = x - kx;
		float dy = y - ky;
		float distance;

		// (wrapped round the picture, then squeezed across the grain)
		dx -= floorf(dx + 0.5f);
		dy -= floorf(dy + 0.5f);
		distance = sqrtf(dx * dx * 9.0f + dy * dy * 16.0f);
		if (distance < 0.12f)
		{
			float ring = 0.5f + 0.5f * sinf(distance * 160.0f);
			float inside = 1.0f - smooth_step(0.04f, 0.12f, distance);

			mix_color(color, color, dark, inside * (0.45f + ring * 0.25f));
		}
	}
	mix_color(color, color, dark, check * 0.75f);
	height = 0.5f + (grain - 0.5f) * 0.25f + (fibre - 0.5f) * 0.16f + (scoop - 0.5f) * 0.16f - smooth_step(0.75f, 0.95f, lines) * 0.12f - check * 0.55f;
	*height_out = clamp_float(height, 0.0f, 1.0f);
	*roughness_out = 0.74f + smooth_step(0.45f, 0.75f, weather) * 0.14f + check * 0.1f;
}

// Boards left to the weather for years (a barn's walls): the timber gone
// silver grey, lighter, the grain still showing, darker streaks under the
// nails and where the rain runs
static void boards_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	float streaks = tiling_fractal(x, y, 12, 3, 3, 31u);
	float patches = tiling_fractal(x, y, 4, 6, 3, 32u);
	float silver[3];
	float warm[3];
	float dark[3];

	timber_texel(x, y, color, height_out, roughness_out);
	set_color(silver, 0.66f, 0.64f, 0.58f);
	set_color(warm, 0.52f, 0.42f, 0.3f);
	set_color(dark, 0.36f, 0.34f, 0.3f);
	// Mostly silver, the warm wood showing through in patches, dark streaks
	mix_color(color, color, silver, 0.6f);
	mix_color(color, color, warm, smooth_step(0.55f, 0.8f, patches) * 0.2f);
	mix_color(color, color, dark, smooth_step(0.6f, 0.85f, streaks) * 0.3f);
	*roughness_out = 0.85f;
}

// A boarded wall (a barn's): vertical boards side by side, each its own
// piece of weathered timber (boards_texel, the grain up its length, shifted
// and lighter or darker by the board), a dark gap between them with the
// boards' edges rounded into it, a row of rusty nails across each board at
// the rails behind (a streak of rust under each). The wall is flat: the
// boards are only here, in the colors, the normals and the occlusion
#define BOARD_WALL_BOARDS 8
static void board_wall_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	const float gap = 0.05f;               // of a board's pitch
	float across = x * (float)BOARD_WALL_BOARDS;
	int board = (int)floorf(across);
	float t = across - (float)board;
	// The boards' edges not quite straight: a slow sway, tiling up the wall
	float sway = (tiling_noise(y * 6.0f, (float)board * 1.7f, 6, 41u + (uint32_t)board) - 0.5f) * 0.03f;
	float shift_along = hash_unit(board, 0, 0, 42u);
	float shift_across = hash_unit(board, 1, 0, 42u);
	float shade = 0.88f + 0.24f * hash_unit(board, 2, 0, 42u);
	float from_gap = fminf(t - gap - sway, 1.0f - t + sway);
	float height;
	float dark[3];
	float rust[3];

	board = ((board % BOARD_WALL_BOARDS) + BOARD_WALL_BOARDS) % BOARD_WALL_BOARDS;
	if (from_gap < 0.0f)
	{
		// The gap: the dark inside, a little light on the boards' sides
		set_color(color, 0.035f, 0.032f, 0.03f);
		*height_out = 0.0f;
		*roughness_out = 1.0f;
		return;
	}
	// The board (the grain along its first coordinate: up the wall here)
	{
		float along = y + shift_along;
		float sideways = x + shift_across * 0.37f;

		boards_texel(along - floorf(along), sideways - floorf(sideways), color, &height, roughness_out);
	}
	for (int k = 0; k < 3; k++)
	{
		color[k] *= shade;
	}
	// Rounded into the gap, darker there (dirt in the edges)
	{
		float edge = smooth_step(0.0f, 0.06f, from_gap);

		height = 0.35f + 0.25f * edge + (height - 0.5f) * 0.2f;
		for (int k = 0; k < 3; k++)
		{
			color[k] *= 0.65f + 0.35f * edge;
		}
	}
	// The nails: two rows up the tile (the rails behind), two nails a board
	set_color(dark, 0.08f, 0.07f, 0.06f);
	set_color(rust, 0.35f, 0.18f, 0.08f);
	for (int row = 0; row < 2; row++)
	{
		float rail_y = row == 0 ? 0.22f : 0.72f;

		for (int nail = 0; nail < 2; nail++)
		{
			float nail_t = nail == 0 ? 0.3f : 0.7f;
			float dx = (t - nail_t) / (float)BOARD_WALL_BOARDS;
			float dy = y - rail_y - (hash_unit(board, 3 + row, nail, 43u) - 0.5f) * 0.01f;
			float head = sqrtf(dx * dx + dy * dy);
			// The rust run down from it (V up the wall: below is smaller y)
			float streak = (1.0f - smooth_step(0.0f, 0.004f, fabsf(dx))) * (dy < 0.0f ? 1.0f - smooth_step(0.0f, 0.06f, -dy) : 0.0f);

			mix_color(color, color, rust, streak * 0.35f);
			if (head < 0.0035f)
			{
				mix_color(color, color, dark, 1.0f - smooth_step(0.002f, 0.0035f, head));
				height += 0.05f * (1.0f - head / 0.0035f);
			}
		}
	}
	*height_out = height;
}

// Clay tiles, beaver tail: rows of flat tiles with rounded lower ends,
// every row half a tile over, each lying over the one below it (the tile
// above covers the top of the one below). V runs down the slope. Each
// tile fired its own shade (red, orange, brown, now and then a dark one),
// its face sandy, its lower end lifted on the tile under it: the shadow it
// casts on that one, and the dark gaps between the tiles of a row
#define ROOF_TILE_ROWS 10
#define ROOF_TILE_COLUMNS 8
#define ROOF_TILE_OVERLAP 0.6f             // how far a tile reaches into the row below (of a row)
#define ROOF_TILE_ROUND 0.45f              // how far up its rounded end goes (of a row)
#define ROOF_TILE_SIDE_GAP 0.03f           // the gap between two tiles of a row (of a tile's width)

// How far down a tile of the row whose top is row_top reaches at x (in
// rows), and where across it x is (0..1) and which column it is
static float roof_tile_reach(float x, int row, float *across_out, int *column_out)
{
	float shift = (row & 1) ? 0.5f : 0.0f;
	float tx_all = x * (float)ROOF_TILE_COLUMNS - shift;
	int column = (int)floorf(tx_all);
	float tx = tx_all - (float)column;
	float across = (tx - 0.5f) * 2.0f;

	*across_out = tx;
	*column_out = ((column % ROOF_TILE_COLUMNS) + ROOF_TILE_COLUMNS) % ROOF_TILE_COLUMNS;
	return 1.0f + ROOF_TILE_OVERLAP - ROOF_TILE_ROUND * (1.0f - sqrtf(fmaxf(0.0f, 1.0f - across * across)));
}

static void roof_tile_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	float rows_y = y * (float)ROOF_TILE_ROWS;
	int base_row = (int)floorf(rows_y);
	int tile_row = 0;
	int tile_column = 0;
	float tile_tx = 0.0f;
	float tile_ty = 0.0f;
	bool found = false;
	float sand = tiling_fractal(x, y, 160, 160, 2, 34u);
	float surface = tiling_fractal(x, y, 24, 24, 3, 36u);
	float grime = tiling_fractal(x, y, 5, 5, 3, 35u);

	// Which tile shows here: the one of the row above lies over this row's,
	// so it is asked first
	for (int candidate = base_row - 1; candidate <= base_row; candidate++)
	{
		int row = ((candidate % ROOF_TILE_ROWS) + ROOF_TILE_ROWS) % ROOF_TILE_ROWS;
		float tx;
		int column;
		float reach = roof_tile_reach(x, row, &tx, &column);
		float ty = rows_y - (float)candidate;

		if (ty < 0.0f || ty > reach || tx < ROOF_TILE_SIDE_GAP || tx > 1.0f - ROOF_TILE_SIDE_GAP)
		{
			continue;
		}
		found = true;
		tile_row = candidate;
		tile_column = column;
		tile_tx = tx;
		tile_ty = ty;
		break;
	}
	if (!found)
	{
		// Between tiles: the deep dark under them
		set_color(color, 0.12f, 0.08f, 0.06f);
		*height_out = 0.05f + sand * 0.05f;
		*roughness_out = 0.95f;
		return;
	}
	{
		int row = ((tile_row % ROOF_TILE_ROWS) + ROOF_TILE_ROWS) % ROOF_TILE_ROWS;
		float pick = hash_unit(tile_column, row, 7, 32u);
		float shade = hash_unit(tile_column, row, 8, 33u);
		float curve = 1.0f - fabsf(tile_tx - 0.5f) * 2.0f;
		float along = tile_ty / (1.0f + ROOF_TILE_OVERLAP);
		float shadow = 0.0f;
		float palette[5][3] =
		{
			{ 0.56f, 0.28f, 0.18f },           // red
			{ 0.6f, 0.33f, 0.2f },             // orange
			{ 0.5f, 0.28f, 0.2f },             // brown
			{ 0.54f, 0.34f, 0.26f },           // faded
			{ 0.38f, 0.23f, 0.18f },           // over fired, dark
		};
		int which = pick < 0.34f ? 0 : (pick < 0.58f ? 1 : (pick < 0.8f ? 2 : (pick < 0.94f ? 3 : 4)));
		float height;

		// The shadow of the tile above on this one, just under its rounded
		// end (where this one's top is not covered)
		{
			int above_row = (((tile_row - 1) % ROOF_TILE_ROWS) + ROOF_TILE_ROWS) % ROOF_TILE_ROWS;
			float above_tx;
			int above_column;
			float above_reach = roof_tile_reach(x, above_row, &above_tx, &above_column);
			float below_its_end = (rows_y - (float)(tile_row - 1)) - above_reach;

			if (below_its_end >= 0.0f && below_its_end < 0.35f)
			{
				shadow = 1.0f - smooth_step(0.0f, 0.35f, below_its_end);
			}
		}
		memcpy(color, palette[which], sizeof(float) * 3);
		for (int k = 0; k < 3; k++)
		{
			color[k] *= 0.9f + shade * 0.12f + (sand - 0.5f) * 0.14f + (surface - 0.5f) * 0.1f + curve * 0.05f;
			color[k] *= 1.0f - shadow * 0.55f;
			color[k] *= 1.0f - smooth_step(0.5f, 0.85f, grime) * 0.18f;
		}
		// Its height: it rests on the tile below, so it rises toward its
		// lower end; a little curved across, sandy, its edges eased
		height = 0.3f + along * 0.45f + curve * 0.06f + (sand - 0.5f) * 0.05f + (surface - 0.5f) * 0.05f + shade * 0.04f;
		height -= (1.0f - smooth_step(0.0f, 0.06f, fminf(tile_tx - ROOF_TILE_SIDE_GAP, 1.0f - ROOF_TILE_SIDE_GAP - tile_tx))) * 0.08f;
		*height_out = clamp_float(height, 0.0f, 1.0f);
		*roughness_out = 0.72f + sand * 0.12f + smooth_step(0.5f, 0.85f, grime) * 0.1f;
	}
}

// Stone laid in courses: rows of stones, each course its own height, each
// stone its own length, mortar between, the stones' faces bumpy and their
// edges worn; courses: how many across the picture (fewer: bigger blocks)
static void coursed_stone_texel(float x, float y, int courses, float variety, uint32_t seed, float color[3], float *height_out, float *roughness_out)
{
	float course_tops[16];
	int course = 0;
	float course_top = 0.0f;
	float course_height = 1.0f;
	float cy;
	int stones;
	float along;
	int stone;
	float sx;
	float stone_width;
	float mortar = 0.018f;
	float face;
	float edge;
	float in_stone;
	float random;
	float bumps = tiling_fractal(x, y, 24, 24, 4, seed + 1u);
	float big_bumps = tiling_fractal(x, y, 6, 6, 2, seed + 2u);
	float pits = smooth_step(0.7f, 0.9f, tiling_fractal(x, y, 48, 48, 2, seed + 3u));
	float grain = tiling_fractal(x, y, 96, 96, 2, seed + 6u);
	float light[3];
	float dark[3];
	float warm[3];
	float cool[3];
	float joint[3];
	float total = 0.0f;

	if (courses > 15)
	{
		courses = 15;
	}
	// The courses' heights, each its own, summing to the picture
	for (int c = 0; c < courses; c++)
	{
		course_tops[c] = total;
		total += 0.7f + 0.6f * hash_unit(c, 0, 9, seed);
	}
	course_tops[courses] = total;
	for (int c = 0; c < courses; c++)
	{
		float top = course_tops[c] / total;
		float bottom = course_tops[c + 1] / total;

		if (y >= top && y < bottom)
		{
			course = c;
			course_top = top;
			course_height = bottom - top;
		}
	}
	cy = (y - course_top) / course_height;
	// The stones along the course, about as long as the course is tall
	// times 1.5 to 2.5, their joints never over the ones below
	stones = (int)((float)courses * 0.55f / (course_height * (float)courses) + 0.5f);
	if (stones < 2)
	{
		stones = 2;
	}
	along = x * (float)stones + hash_unit(course, 0, 1, seed);
	stone = (int)floorf(along);
	sx = along - (float)stone;
	{
		int wrapped = ((stone % stones) + stones) % stones;
		int next = (((stone + 1) % stones) + stones) % stones;
		float push_left = (hash_unit(wrapped, course, 2, seed) - 0.5f) * 0.7f;
		float push_right = (hash_unit(next, course, 2, seed) - 0.5f) * 0.7f;

		stone_width = 1.0f + push_right - push_left;
		sx = (sx - push_left) / stone_width;
		if (sx < 0.0f)
		{
			stone--;
			sx += 1.0f;
		}
		else if (sx > 1.0f)
		{
			stone++;
			sx -= 1.0f;
		}
		stone = ((stone % stones) + stones) % stones;
	}
	random = hash_unit(stone, course, 3, seed);
	// Mortar at the stone's edges (the joints wander a little), its face
	// domed and worn round at the edges
	{
		float wander = (tiling_fractal(x, y, 40, 40, 2, seed + 4u) - 0.5f) * 0.03f;
		float edge_x = fminf(sx, 1.0f - sx) * (stone_width * (float)stones / (float)courses) * course_height * (float)courses / 1.6f;
		float edge_y = fminf(cy, 1.0f - cy);

		// (the corners rounded: the two distances blended where both are small)
		float corner = 0.035f;
		float rounded = edge_x < corner && edge_y < corner
			? corner - sqrtf((corner - edge_x) * (corner - edge_x) + (corner - edge_y) * (corner - edge_y)) : fminf(edge_x, edge_y);

		edge = rounded + wander;
		in_stone = smooth_step(mortar * 0.8f, mortar * 1.5f, edge);
	}
	// The face: domed from its edges, dressed with a chisel (fine strokes
	// slanting across it), bumpy and pitted; the mortar well back
	{
		float dome = sqrtf(clamp_float((edge - mortar) / 0.12f, 0.0f, 1.0f));
		float strokes = tiling_fractal(x + y * 0.2f, y, 140, 18, 2, seed + 7u);

		face = 0.15f + in_stone * (0.42f + dome * 0.18f) + (big_bumps - 0.5f) * 0.22f * in_stone + (bumps - 0.5f) * 0.26f * in_stone
			+ (grain - 0.5f) * 0.12f * in_stone + (strokes - 0.5f) * 0.1f * in_stone + random * 0.1f * in_stone
			+ (1.0f - in_stone) * (grain - 0.5f) * 0.1f - pits * 0.12f * in_stone;
	}
	set_color(light, 0.64f, 0.6f, 0.53f);
	set_color(dark, 0.4f, 0.38f, 0.35f);
	set_color(warm, 0.58f, 0.48f, 0.38f);
	set_color(cool, 0.48f, 0.5f, 0.5f);
	set_color(joint, 0.46f, 0.42f, 0.37f);
	mix_color(color, light, dark, 0.3f + (random - 0.5f) * variety);
	mix_color(color, color, warm, smooth_step(0.55f, 0.85f, hash_unit(stone, course, 4, seed)) * variety);
	mix_color(color, color, cool, smooth_step(0.6f, 0.9f, hash_unit(stone, course, 5, seed)) * variety * 0.8f);
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.8f + bumps * 0.25f + (grain - 0.5f) * 0.2f - pits * 0.15f;
	}
	// (darker into the joint: the stone's edge in its shadow, dirt there)
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.82f + 0.18f * smooth_step(mortar, mortar * 4.0f, edge);
	}
	// The mortar: lime and sand, lighter, gritty
	{
		float sandy[3];

		for (int k = 0; k < 3; k++)
		{
			sandy[k] = joint[k] * (0.85f + grain * 0.3f);
		}
		mix_color(color, sandy, color, in_stone);
	}
	*height_out = clamp_float(face, 0.0f, 1.0f);
	*roughness_out = 0.78f + (1.0f - in_stone) * 0.17f + pits * 0.05f;
}

static void base_stone_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	coursed_stone_texel(x, y, 6, 0.45f, 41u, color, height_out, roughness_out);
	// The base is greyer and darker: it stands in the wet
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.8f;
	}
}

static void wall_stone_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	coursed_stone_texel(x, y, 7, 0.75f, 42u, color, height_out, roughness_out);
}

// Thatch of water reed: what shows is the reeds' butt ends, combed tight,
// a dense bristle, laid in courses down the slope (V), each course's lower
// edge standing out over the next a little and ragged; bundle by bundle a
// shade lighter or darker, golden where it is newer, grey-brown where the
// weather has had it. The moss is the building shader's
#define THATCH_COURSES 6
static void thatch_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	// The course: its lower edge wanders, bundle by bundle
	float edge_wander = (tiling_fractal(x, y, 12, THATCH_COURSES, 2, 55u) - 0.5f) * 0.14f;
	float course_y = y * (float)THATCH_COURSES + edge_wander;
	float in_course = course_y - floorf(course_y);
	int course = (int)floorf(course_y);
	// The bundles across a course, each its own shade
	float bundle_x = x * 28.0f + hash_unit(course, 0, 1, 56u) * 5.0f;
	int bundle = (int)floorf(bundle_x);
	float bundle_shade = hash_unit(((bundle % 28) + 28) % 28, ((course % THATCH_COURSES) + THATCH_COURSES) % THATCH_COURSES, 2, 57u);
	// The reeds: fine lines down the slope, their ends a speckle
	float reeds = tiling_fractal(x, y, 360, 12, 2, 51u);
	float ends = tiling_fractal(x, y, 420, 420, 2, 52u);
	float clumps = tiling_fractal(x, y, 40, 20, 3, 53u);
	float weathering = tiling_fractal(x, y, 3, 3, 3, 54u);
	float fresh[3];
	float grey[3];
	float deep[3];
	float height;
	// Down the course it lies further out: the next course's top is under it
	float lift = smooth_step(0.0f, 0.9f, in_course);
	// Its lower edge: the ends stand out, the shadow under them on the next
	float under_edge = 1.0f - smooth_step(0.0f, 0.22f, in_course);

	set_color(fresh, 0.64f, 0.54f, 0.34f);
	set_color(grey, 0.45f, 0.41f, 0.34f);
	set_color(deep, 0.16f, 0.13f, 0.09f);
	mix_color(color, fresh, grey, smooth_step(0.35f, 0.75f, weathering) * 0.55f + (bundle_shade - 0.5f) * 0.3f);
	// Between the reeds it is dark, deep in the bristle
	mix_color(color, color, deep, smooth_step(0.35f, 0.8f, 1.0f - reeds) * 0.4f + (1.0f - clumps) * 0.12f);
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.85f + bundle_shade * 0.2f + smooth_step(0.7f, 0.9f, ends) * 0.2f;
		color[k] *= 1.0f - under_edge * under_edge * 0.14f;
	}
	height = 0.4f + lift * 0.16f + (reeds - 0.5f) * 0.32f + (clumps - 0.5f) * 0.15f + smooth_step(0.7f, 0.9f, ends) * 0.06f - under_edge * 0.08f;
	*height_out = clamp_float(height, 0.0f, 1.0f);
	*roughness_out = 0.96f;
}

// Daub: clay and lime plastered by hand over wattle, the wash gone
// yellow-grey, chopped straw showing in it, shrinkage cracks, fallen off
// here and there to the wattle: rods woven across (along U) in and out
// of stakes standing up (along V), the daub's broken edge round the hole
#define DAUB_RODS 36
#define DAUB_STAKES 10
static void daub_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	float unevenness = tiling_fractal(x, y, 5, 5, 4, 61u);
	float lumps = tiling_fractal(x, y, 20, 20, 3, 66u);
	float grain = tiling_fractal(x, y, 160, 160, 2, 62u);
	float holes = tiling_fractal(x, y, 3, 3, 4, 63u);
	float fallen = smooth_step(0.7f, 0.73f, holes);
	float rim = smooth_step(0.65f, 0.7f, holes) * (1.0f - fallen);
	float cracks = (1.0f - smooth_step(0.0f, 0.03f, tiling_cracks(x, y, 7, 64u))) * smooth_step(0.4f, 0.6f, tiling_fractal(x, y, 4, 4, 2, 67u));
	// The straw: short bright flecks every way, from two stretched noises
	float straw = fmaxf(smooth_step(0.8f, 0.86f, tiling_fractal(x, y, 220, 30, 2, 68u)),
		smooth_step(0.8f, 0.86f, tiling_fractal(x, y, 30, 220, 2, 69u))) * smooth_step(0.5f, 0.7f, tiling_fractal(x, y, 60, 60, 2, 70u));
	float wash[3];
	float clay[3];
	float straw_color[3];
	float height;

	set_color(wash, 0.78f, 0.72f, 0.6f);
	set_color(clay, 0.56f, 0.46f, 0.34f);
	set_color(straw_color, 0.72f, 0.62f, 0.4f);
	mix_color(color, wash, clay, 0.25f + (1.0f - unevenness) * 0.35f + (lumps - 0.5f) * 0.2f);
	mix_color(color, color, straw_color, straw * 0.6f);
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.92f + grain * 0.14f;
		color[k] *= 1.0f - cracks * 0.4f;
		color[k] *= 1.0f - rim * 0.15f;
	}
	height = 0.55f + (unevenness - 0.5f) * 0.45f + (lumps - 0.5f) * 0.2f + (grain - 0.5f) * 0.06f - cracks * 0.2f + rim * 0.06f;
	// Where it fell off: the wattle, deeper, darker
	if (fallen > 0.0f)
	{
		float rod_y = y * (float)DAUB_RODS;
		int rod = (int)floorf(rod_y);
		float in_rod = rod_y - (float)rod;
		float round_rod = sqrtf(fmaxf(0.0f, 1.0f - (in_rod - 0.5f) * (in_rod - 0.5f) * 4.0f));
		// In front of a stake, then behind the next: every other rod the other way
		float weave = 0.5f + 0.5f * sinf(x * (float)DAUB_STAKES * 3.14159265f * 2.0f + (float)rod * 3.14159265f);
		float stake_x = x * (float)DAUB_STAKES;
		float stake = 1.0f - smooth_step(0.06f, 0.12f, fabsf(stake_x - floorf(stake_x) - 0.25f));
		float rod_color[3];
		float rod_height = 0.12f + round_rod * 0.1f + weave * 0.1f;
		float grain_along = tiling_fractal(x, y, 40, DAUB_RODS * 4, 2, 71u);

		set_color(rod_color, 0.4f, 0.31f, 0.2f);
		for (int k = 0; k < 3; k++)
		{
			rod_color[k] *= (0.5f + round_rod * 0.4f + weave * 0.2f) * (0.85f + grain_along * 0.3f);
		}
		// (a stake shows where the rods pass behind it)
		if (stake > 0.0f && weave < 0.5f)
		{
			float stake_color[3];

			set_color(stake_color, 0.36f, 0.28f, 0.19f);
			mix_color(rod_color, rod_color, stake_color, stake);
			rod_height = fmaxf(rod_height, 0.2f * stake);
		}
		mix_color(color, color, rod_color, fallen);
		height = height * (1.0f - fallen) + rod_height * fallen;
	}
	*height_out = clamp_float(height, 0.0f, 1.0f);
	*roughness_out = 0.92f;
}

// Rubble: field stones of every size and shape, wide joints of mud and
// lime between them
static void rubble_texel(float x, float y, float color[3], float *height_out, float *roughness_out)
{
	const int cells = 7;
	float px = x * (float)cells;
	float py = y * (float)cells;
	int ix = (int)floorf(px);
	int iy = (int)floorf(py);
	float nearest = 9.0f;
	float second = 9.0f;
	int nearest_x = 0;
	int nearest_y = 0;
	float bumps = tiling_fractal(x, y, 24, 24, 4, 71u);
	float grain = tiling_fractal(x, y, 110, 110, 2, 72u);
	float random;
	float edge;
	float in_stone;
	float face;
	float light[3];
	float dark[3];
	float warm[3];
	float joint[3];

	// The nearest two stones' seats, the stones' sizes differing
	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int cx = ix + dx;
			int cy = iy + dy;
			int wx = ((cx % cells) + cells) % cells;
			int wy = ((cy % cells) + cells) % cells;
			uint32_t h = hash_cell(wx, wy, 5, 73u);
			float sx = (float)cx + (float)(h & 0xFFu) / 255.0f;
			float sy = (float)cy + (float)((h >> 8) & 0xFFu) / 255.0f;
			// Flatter than tall: field stones lie
			float distance = sqrtf((px - sx) * (px - sx) * 0.6f + (py - sy) * (py - sy) * 1.4f) * (0.8f + 0.4f * hash_unit(wx, wy, 6, 73u));

			if (distance < nearest)
			{
				second = nearest;
				nearest = distance;
				nearest_x = wx;
				nearest_y = wy;
			}
			else if (distance < second)
			{
				second = distance;
			}
		}
	}
	edge = (second - nearest);
	in_stone = smooth_step(0.08f, 0.3f, edge);
	random = hash_unit(nearest_x, nearest_y, 3, 74u);
	face = 0.25f + 0.5f * in_stone * (1.0f - nearest * 0.35f) + (bumps - 0.5f) * 0.3f * in_stone + (grain - 0.5f) * 0.12f + random * 0.1f * in_stone
		+ (1.0f - in_stone) * (bumps - 0.5f) * 0.15f;
	set_color(light, 0.6f, 0.57f, 0.5f);
	set_color(dark, 0.36f, 0.35f, 0.33f);
	set_color(warm, 0.55f, 0.44f, 0.33f);
	set_color(joint, 0.5f, 0.44f, 0.36f);
	mix_color(color, light, dark, random);
	mix_color(color, color, warm, smooth_step(0.6f, 0.9f, hash_unit(nearest_x, nearest_y, 4, 74u)) * 0.7f);
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.8f + bumps * 0.25f + (grain - 0.5f) * 0.2f;
	}
	mix_color(color, joint, color, in_stone);
	*height_out = clamp_float(face, 0.0f, 1.0f);
	*roughness_out = 0.85f + (1.0f - in_stone) * 0.1f;
}

static const stand_in_t stand_ins[] =
{
	{ "daub", daub_texel, 0.7f },
	{ "rubble", rubble_texel, 1.4f },
	{ "thatch", thatch_texel, 0.8f },
	{ "plaster", plaster_texel, 0.35f },
	{ "timber", timber_texel, 0.6f },
	{ "board_wall", board_wall_texel, 0.5f },
	{ "roof_tile", roof_tile_texel, 1.2f },
	{ "base_stone", base_stone_texel, 1.4f },
	{ "wall_stone", wall_stone_texel, 1.3f },
};

// ---------------------------------------------------------------------------

static float to_byte(float value)
{
	return clamp_float(value * 255.0f + 0.5f, 0.0f, 255.0f);
}

static void write_to_file(void *context, void *data, int size)
{
	fwrite(data, 1, (size_t)size, (FILE *)context);
}

static bool write_png(const char *path, const uint8_t *pixels, int width, int height)
{
	FILE *file = fopen(path, "wb");
	int ok;

	if (file == NULL)
	{
		return false;
	}
	ok = stbi_write_png_to_func(write_to_file, file, width, height, 4, pixels, width * 4);
	fclose(file);
	return ok != 0;
}

static bool file_exists(const char *path)
{
	struct stat info;

	return stat(path, &info) == 0;
}

static void make_folders(const char *path)
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

// The plaster's tints: the timber towns' colors, over the lime
typedef struct
{
	const char *name;
	float tint[3];
} tint_t;

static const tint_t tints[] =
{
	{ "salmon", { 1.0f, 0.68f, 0.56f } },
	{ "sage", { 0.76f, 0.86f, 0.66f } },
	{ "ochre", { 1.0f, 0.86f, 0.55f } },
	{ "blue", { 0.68f, 0.8f, 0.94f } },
	{ "cream", { 1.0f, 0.96f, 0.84f } },
	{ "pink", { 0.96f, 0.76f, 0.74f } },
};

int house_tint_count(void)
{
	return (int)(sizeof(tints) / sizeof(tints[0]));
}

const char *house_tint_name(int index)
{
	return index >= 0 && index < house_tint_count() ? tints[index].name : NULL;
}

bool house_stand_in_textures(const char *name, const char *data_directory, bool remake, char names_out[4][HOUSE_PATH_LENGTH])
{
	static const char *const suffixes[3] = { "color", "normal", "arm" };
	const stand_in_t *stand_in = NULL;
	const tint_t *tint = NULL;
	char base_name[64];

	// "plaster_salmon" and the like: the plaster, tinted
	snprintf(base_name, sizeof(base_name), "%s", name);
	if (strncmp(name, "plaster_", 8) == 0)
	{
		for (int i = 0; i < house_tint_count(); i++)
		{
			if (strcmp(name + 8, tints[i].name) == 0)
			{
				tint = &tints[i];
				snprintf(base_name, sizeof(base_name), "plaster");
			}
		}
	}
	char paths[3][HOUSE_PATH_LENGTH * 2];
	bool missing = remake;
	float *heights;
	float *roughness;
	uint8_t *maps[3];
	int size = TEXTURE_SIZE;

	for (size_t i = 0; i < sizeof(stand_ins) / sizeof(stand_ins[0]); i++)
	{
		if (strcmp(stand_ins[i].name, base_name) == 0)
		{
			stand_in = &stand_ins[i];
		}
	}
	if (stand_in == NULL)
	{
		return false;
	}
	for (int m = 0; m < 3; m++)
	{
		snprintf(names_out[m], HOUSE_PATH_LENGTH, "props/textures/house_%s_%s.png", name, suffixes[m]);
		snprintf(paths[m], sizeof(paths[m]), "%s/%s", data_directory, names_out[m]);
		missing = missing || !file_exists(paths[m]);
	}
	names_out[3][0] = '\0';
	if (!missing)
	{
		return true;
	}
	make_folders(paths[0]);
	heights = malloc(sizeof(float) * (size_t)size * (size_t)size);
	roughness = malloc(sizeof(float) * (size_t)size * (size_t)size);
	for (int m = 0; m < 3; m++)
	{
		maps[m] = malloc((size_t)size * (size_t)size * 4);
	}
	if (heights == NULL || roughness == NULL || maps[0] == NULL || maps[1] == NULL || maps[2] == NULL)
	{
		free(heights);
		free(roughness);
		for (int m = 0; m < 3; m++)
		{
			free(maps[m]);
		}
		return false;
	}
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float color[3];
			float height;
			float rough;
			size_t here = (size_t)y * (size_t)size + (size_t)x;
			uint8_t *texel = &maps[0][here * 4];

			stand_in->texel(((float)x + 0.5f) / (float)size, ((float)y + 0.5f) / (float)size, color, &height, &rough);
			if (tint != NULL)
			{
				// Lime wash over the color: half way back to white
				for (int k = 0; k < 3; k++)
				{
					color[k] *= 1.0f - (1.0f - tint->tint[k]) * 0.55f;
				}
			}
			heights[here] = height;
			roughness[here] = rough;
			for (int k = 0; k < 3; k++)
			{
				texel[k] = (uint8_t)to_byte(color[k]);
			}
			texel[3] = 255;
		}
	}
	// The normals and the occlusion from the heights (wrapping round: it
	// tiles)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			size_t here = (size_t)y * (size_t)size + (size_t)x;
			float left = heights[(size_t)y * (size_t)size + (size_t)((x + size - 1) % size)];
			float right = heights[(size_t)y * (size_t)size + (size_t)((x + 1) % size)];
			float up = heights[(size_t)((y + size - 1) % size) * (size_t)size + (size_t)x];
			float down = heights[(size_t)((y + 1) % size) * (size_t)size + (size_t)x];
			float strength = stand_in->bumps * 6.0f;
			// (the rows run down: the texture's V; OpenGL's normal maps: green up)
			float nx = (left - right) * strength;
			float ny = (down - up) * strength;
			float nz = 1.0f;
			float length = sqrtf(nx * nx + ny * ny + nz * nz);
			float around = 0.0f;
			float occlusion;
			uint8_t *n = &maps[1][here * 4];
			uint8_t *a = &maps[2][here * 4];

			// Occlusion: how much higher the neighborhood is, a few texels out
			for (int r = 1; r <= 3; r++)
			{
				int step = r * 3;

				around += heights[(size_t)y * (size_t)size + (size_t)((x + size - step) % size)];
				around += heights[(size_t)y * (size_t)size + (size_t)((x + step) % size)];
				around += heights[(size_t)((y + size - step) % size) * (size_t)size + (size_t)x];
				around += heights[(size_t)((y + step) % size) * (size_t)size + (size_t)x];
			}
			around /= 12.0f;
			occlusion = clamp_float(1.0f - (around - heights[here]) * 2.5f, 0.45f, 1.0f);
			n[0] = (uint8_t)to_byte(nx / length * 0.5f + 0.5f);
			n[1] = (uint8_t)to_byte(ny / length * 0.5f + 0.5f);
			n[2] = (uint8_t)to_byte(nz / length * 0.5f + 0.5f);
			// (the height in the normal map's alpha only for the thatch,
			// whose relief is marched into by it: a varying alpha costs the
			// normals their precision in the GPU's compression, BC7 sharing
			// a block's bits with it)
			n[3] = strcmp(name, "thatch") == 0 ? (uint8_t)to_byte(heights[here]) : 255;
			a[0] = (uint8_t)to_byte(occlusion);
			a[1] = (uint8_t)to_byte(roughness[here]);
			a[2] = 0;
			a[3] = 255;
		}
	}
	{
		bool ok = write_png(paths[0], maps[0], size, size) && write_png(paths[1], maps[1], size, size) && write_png(paths[2], maps[2], size, size);

		free(heights);
		free(roughness);
		for (int m = 0; m < 3; m++)
		{
			free(maps[m]);
		}
		return ok;
	}
}
