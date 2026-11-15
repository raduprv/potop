// potop_texture: makes the ground's textures (data/materials.ini) from
// nothing: a color, its normal map and its height, tiling by construction
// (every noise in it repeats over the picture), at any size. A desktop tool:
// it is not part of the game and never ships with it.
//
//   potop_texture [options] KIND NAME
//
// writes data/textures/NAME.png, NAME_normal.png (green up the picture,
// OpenGL's) and NAME_height.png. Kinds:
//   sand    wind ripples (two sets of them crossing, their crests winding,
//           breaking off, fading out in patches), grains of several minerals
//           (most of them quartz and feldspar, a few dark heavy ones gathered
//           in the troughs), coarser grains, bits of shell
//   clay    dry clay cracked into plates (big cracks and finer ones inside
//           them, the plates' edges curling up), its crust's fine grit, a
//           few grains and pebbles on it
//   rock    a weathered stone face: broken into facets and blocks along
//           fractures, chipped, its minerals speckled, stained in streaks,
//           dark in its cracks (a cliff's, the bare rock's)
//   dirt    bare earth: crumbs and clods, small stones, bits of dead plants
//   mud     clay soaked (materials.ini's wet_texture for clay): smooth, its
//           cracks closed where the clay of the same seed has them, ripple
//           marks, drops' pits, puddles
//   grass   grass from above: blades in tufts, leaning together in patches,
//           dead ones under the living, soil and thatch between
//   stone   granite's massive stone without lichen: a rock's up close
//           (materials.ini: under a macro_texture of beds)
//   relief  a rock face's big shapes (96 m: gullies, buttresses, ribs), only
//           its normals and height: materials.ini's relief
//   needles a conifer's litter: needles layer on layer (the old gray, the
//           new rust), a cone here and there, humus between
//   leaves  a broadleaved tree's: leaves (the rotten ones dark under the
//           newly fallen, their autumn colors), humus between
//   trampled  earth pressed flat round houses and along paths, dead grass
//           pressed into it
//   petals  a blooming tree's petals on the ground, and NAME_mask.png: in
//           what order they show as more of them fall (an overlay's)
//   snow    soft drifts, wind crust, and NAME_mask.png: where it settles
//           first (patches, then all of it)
//   drystone  a dry stone wall's face (a wooden church's footing): field
//           stones of every size, no courses, river stones and limestone,
//           lichen, earth in the joints; NAME_height.png (its relief, for the
//           house generator to push its footing out by) and NAME_arm.png
//   coursed a coursed stone wall: limestone roughly squared, in courses,
//           thin ones two to a course here and there, hammered faces,
//           pitched edges, lime mortar (and NAME_height.png, NAME_arm.png)
//   weatherboard  old boards across the picture, each one's waney lower
//           edge over the next: the grain round its knots, checks, hand
//           made nails and their rust, lichen along the edges, mold (and
//           NAME_height.png, NAME_arm.png; --color: the wood's)
//
// Options:
//   --data DIR         the game's data directory (data)
//   --size N           pixels per side (2048)
//   --meters M         how much ground the picture covers (materials.ini's
//                      tiling_meters should say the same; sand 2, clay 3,
//                      rock 4, dirt 2)
//   --seed N           which one (1)
//   --color R,G,B      its color on the whole (sRGB 0..1; each kind its own)
//   --grain MM         sand: its grains' size (millimeters; 1)
//   --ripples M        sand: the ripples' spacing (meters; 0.11; 0: none)
//   --ripple-height M  sand: their height (meters; 0.008)
//   --plates M         clay: the big plates' size (meters; 0.35)
//   --boards M         weatherboard: what shows of each board (meters; 0.2)
//   --moss A           rock, granite: how much moss or lichen; weatherboard:
//                      how much lichen (0..1; each kind its own)
//   --baked-light A    how much of the light is baked into the color, as a
//                      photograph has it (0..1; 0.4: the game lights the
//                      normals as well)

// (M_PI: POSIX)
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image_write.h"

#define PI_F 3.14159265358979f

typedef struct
{
	int size;
	float meters;
	bool meters_given;
	uint32_t seed;
	float color[3];
	bool color_given;
	float grain_millimeters;
	float ripple_spacing;
	float ripple_height;
	float plate_meters;
	float board_meters;                // weatherboard: what shows of each board (meters)
	float baked_light;                 // how much of the light is in the color (0..1)
	float moss;                        // rock, granite: how much moss or lichen (0..1; below 0: the kind's)
	float puddles;                     // wet_*: the share of it under standing water (0..1)
	bool write_height;                 // NAME_height.png as well (a relief's always)
} texture_settings_t;

// What a texture is made in: its height (meters) and its color (sRGB 0..1)
// at each pixel
typedef struct
{
	int size;
	float meters;
	float *height;
	float *color;
	float *roughness;                  // 0 a mirror .. 1 matte
	float *mask;                       // an overlay's (petals): in what order it shows (NAME_mask.png; NULL: none)
} field_t;

// ---------------------------------------------------------------------------
// Hashes and noise that repeat over the picture (u, v 0..1 across it)

static uint32_t hash_u32(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

static uint32_t hash_cell(int x, int y, uint32_t seed)
{
	return hash_u32((uint32_t)x * 0x9E3779B1u ^ hash_u32((uint32_t)y * 0x85EBCA77u ^ seed));
}

static float hash_unit(int x, int y, uint32_t seed)
{
	return (float)(hash_cell(x, y, seed) & 0xFFFFFFu) / 16777215.0f;
}

static int wrap_index(int value, int period)
{
	return ((value % period) + period) % period;
}

static float fade_curve(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float clamp_unit(float value)
{
	return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static float smooth_step(float edge0, float edge1, float value)
{
	float t = clamp_unit((value - edge0) / (edge1 - edge0));

	return t * t * (3.0f - 2.0f * t);
}

static float mix_float(float a, float b, float t)
{
	return a + (b - a) * t;
}

// Gradient noise at a point in cells, repeating every period cells: about
// -1..1
static float cell_noise(float px, float py, int period, uint32_t seed)
{
	int ix = (int)floorf(px);
	int iy = (int)floorf(py);
	float fx = px - (float)ix;
	float fy = py - (float)iy;
	float dots[4];

	for (int k = 0; k < 4; k++)
	{
		int cx = ix + (k & 1);
		int cy = iy + (k >> 1);
		float angle = hash_unit(wrap_index(cx, period), wrap_index(cy, period), seed) * 2.0f * PI_F;

		dots[k] = cosf(angle) * (fx - (float)(k & 1)) + sinf(angle) * (fy - (float)(k >> 1));
	}
	{
		float u = fade_curve(fx);
		float v = fade_curve(fy);
		float top = dots[0] + (dots[1] - dots[0]) * u;
		float bottom = dots[2] + (dots[3] - dots[2]) * u;

		return (top + (bottom - top) * v) * 1.4f;
	}
}

// The same on the picture, period cells across it; every other octave on a
// lattice turned 45 degrees (still repeating over the picture: u + v and
// v - u move by whole periods when u or v does), so no octave lines up with
// the picture's axes and no grid shows
static float tiling_noise(float u, float v, int period, int turned, uint32_t seed)
{
	if (turned)
	{
		return cell_noise((u + v) * (float)period, (v - u) * (float)period, period, seed);
	}
	return cell_noise(u * (float)period, v * (float)period, period, seed);
}

// Octaves from period cells up, each twice as fine: about -1..1
static float tiling_fractal(float u, float v, int period, int octaves, float falloff, uint32_t seed)
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float total = 0.0f;

	for (int o = 0; o < octaves; o++)
	{
		sum += amplitude * tiling_noise(u, v, period << o, (o + (int)(seed & 1u)) & 1, seed + (uint32_t)o * 7919u);
		total += amplitude;
		amplitude *= falloff;
	}
	return sum / total;
}

// Cells (Voronoi) repeating over the picture, cells across it: the nearest
// point's distance (in cells), the next one's, the nearest's cell and where
// the point is from its middle (in cells)
typedef struct
{
	float nearest;
	float second;
	int cell_x;
	int cell_y;
	float from_x;
	float from_y;
} cells_t;

static cells_t tiling_cells(float u, float v, int cells, float jitter, uint32_t seed)
{
	float cx = u * (float)cells;
	float cy = v * (float)cells;
	int ix = (int)floorf(cx);
	int iy = (int)floorf(cy);
	cells_t result = { 1e9f, 1e9f, 0, 0, 0.0f, 0.0f };

	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int gx = wrap_index(ix + dx, cells);
			int gy = wrap_index(iy + dy, cells);
			float px = (float)(ix + dx) + 0.5f + (hash_unit(gx, gy, seed) - 0.5f) * jitter;
			float py = (float)(iy + dy) + 0.5f + (hash_unit(gx, gy, seed + 1u) - 0.5f) * jitter;
			float ox = cx - px;
			float oy = cy - py;
			float distance = sqrtf(ox * ox + oy * oy);

			if (distance < result.nearest)
			{
				result.second = result.nearest;
				result.nearest = distance;
				result.cell_x = gx;
				result.cell_y = gy;
				result.from_x = ox;
				result.from_y = oy;
			}
			else if (distance < result.second)
			{
				result.second = distance;
			}
		}
	}
	return result;
}

// How far a point is from its cell's edge (in cells): the distance to the
// nearest border between it and a neighbor (not F2 - F1: that is not a
// distance, and makes cracks of uneven width)
static float tiling_cell_edge(float u, float v, int cells, float jitter, uint32_t seed, int *cell_x_out, int *cell_y_out)
{
	float cx = u * (float)cells;
	float cy = v * (float)cells;
	int ix = (int)floorf(cx);
	int iy = (int)floorf(cy);
	float best = 1e9f;
	float best_x = 0.0f;
	float best_y = 0.0f;
	int best_cell[2] = { 0, 0 };
	float edge = 1e9f;

	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int gx = wrap_index(ix + dx, cells);
			int gy = wrap_index(iy + dy, cells);
			float px = (float)(ix + dx) + 0.5f + (hash_unit(gx, gy, seed) - 0.5f) * jitter;
			float py = (float)(iy + dy) + 0.5f + (hash_unit(gx, gy, seed + 1u) - 0.5f) * jitter;
			float d = (cx - px) * (cx - px) + (cy - py) * (cy - py);

			if (d < best)
			{
				best = d;
				best_x = px;
				best_y = py;
				best_cell[0] = gx;
				best_cell[1] = gy;
			}
		}
	}
	for (int dy = -2; dy <= 2; dy++)
	{
		for (int dx = -2; dx <= 2; dx++)
		{
			int gx = wrap_index(ix + dx, cells);
			int gy = wrap_index(iy + dy, cells);
			float px = (float)(ix + dx) + 0.5f + (hash_unit(gx, gy, seed) - 0.5f) * jitter;
			float py = (float)(iy + dy) + 0.5f + (hash_unit(gx, gy, seed + 1u) - 0.5f) * jitter;
			float mx = (best_x + px) * 0.5f;
			float my = (best_y + py) * 0.5f;
			float nx = px - best_x;
			float ny = py - best_y;
			float length = sqrtf(nx * nx + ny * ny);

			if (length < 1e-5f)
			{
				continue;
			}
			edge = fminf(edge, ((mx - cx) * nx + (my - cy) * ny) / length);
		}
	}
	if (cell_x_out != NULL)
	{
		*cell_x_out = best_cell[0];
		*cell_y_out = best_cell[1];
	}
	return edge;
}

// ---------------------------------------------------------------------------
// Working on the whole field

static bool field_create(field_t *field, int size, float meters)
{
	field->size = size;
	field->meters = meters;
	field->height = calloc((size_t)size * (size_t)size, sizeof(float));
	field->color = calloc((size_t)size * (size_t)size * 3, sizeof(float));
	field->roughness = calloc((size_t)size * (size_t)size, sizeof(float));
	field->mask = NULL;
	if (field->height == NULL || field->color == NULL || field->roughness == NULL)
	{
		free(field->height);
		free(field->color);
		free(field->roughness);
		fprintf(stderr, "potop_texture: out of memory\n");
		return false;
	}
	return true;
}

static void field_destroy(field_t *field)
{
	free(field->height);
	free(field->color);
	free(field->roughness);
	free(field->mask);
}

// The whole field's roughness one value (each kind's ground, before its
// pebbles, puddles and moss)
static void fill_roughness(field_t *field, float roughness)
{
	for (int i = 0; i < field->size * field->size; i++)
	{
		field->roughness[i] = roughness;
	}
}

// A blur, wrapping round (a few passes of 1 4 6 4 1 each way: near a
// Gaussian, round, no box's squares)
static void blur_wrapping(float *values, int size, int passes)
{
	for (int p = 0; p < passes; p++)
	{
#pragma omp parallel for schedule(static)
		for (int y = 0; y < size; y++)
		{
			float row[8192];

			for (int x = 0; x < size; x++)
			{
				const float *r = &values[y * size];

				row[x] = (r[wrap_index(x - 2, size)] + 4.0f * r[wrap_index(x - 1, size)] + 6.0f * r[x] + 4.0f * r[wrap_index(x + 1, size)]
					+ r[wrap_index(x + 2, size)]) / 16.0f;
			}
			memcpy(&values[y * size], row, (size_t)size * sizeof(float));
		}
#pragma omp parallel for schedule(static)
		for (int x = 0; x < size; x++)
		{
			float column[8192];

			for (int y = 0; y < size; y++)
			{
				column[y] = (values[wrap_index(y - 2, size) * size + x] + 4.0f * values[wrap_index(y - 1, size) * size + x]
					+ 6.0f * values[y * size + x] + 4.0f * values[wrap_index(y + 1, size) * size + x]
					+ values[wrap_index(y + 2, size) * size + x]) / 16.0f;
			}
			for (int y = 0; y < size; y++)
			{
				values[y * size + x] = column[y];
			}
		}
	}
}

// The shade where the light does not reach, in hollows under their
// surroundings (a photograph's albedo has it: the game lights only the
// normals, which are smoother than the small relief): darker by how far a
// pixel is under its neighbors' average, of depth
static void shade_hollows(field_t *field, int passes, float depth, float strength)
{
	int size = field->size;
	float *around = malloc((size_t)size * (size_t)size * sizeof(float));

	if (around == NULL)
	{
		return;
	}
	memcpy(around, field->height, (size_t)size * (size_t)size * sizeof(float));
	blur_wrapping(around, size, passes);
#pragma omp parallel for schedule(static)
	for (int i = 0; i < size * size; i++)
	{
		float under = fmaxf(around[i] - field->height[i], 0.0f) / fmaxf(depth, 1e-6f);
		float light = 1.0f - strength * (1.0f - expf(-under));

		for (int k = 0; k < 3; k++)
		{
			field->color[i * 3 + k] *= light;
		}
	}
	free(around);
}

// On the whole as light as the color asked for (each channel's average)
static void match_average(field_t *field, const float color[3])
{
	int size = field->size;
	double sums[3] = { 0.0, 0.0, 0.0 };

	for (int i = 0; i < size * size; i++)
	{
		for (int k = 0; k < 3; k++)
		{
			sums[k] += field->color[i * 3 + k];
		}
	}
	for (int k = 0; k < 3; k++)
	{
		float gain = color[k] / fmaxf((float)(sums[k] / ((double)size * (double)size)), 1e-4f);

		for (int i = 0; i < size * size; i++)
		{
			field->color[i * 3 + k] *= gain;
		}
	}
}

// Some of the light baked in, as a photograph has it: the relief lit from
// up the picture's left, low (amount: 0 none .. 1 all of it; the game
// lights the normals too, so only a part)
static void bake_light(field_t *field, float amount)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	float light[3] = { -0.5f, -0.5f, 0.71f };
	float flat = light[2];

	if (amount <= 0.0f)
	{
		return;
	}
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float slope_x = (field->height[y * size + wrap_index(x + 1, size)] - field->height[y * size + wrap_index(x - 1, size)])
				/ (2.0f * pixel_meters);
			float slope_y = (field->height[wrap_index(y + 1, size) * size + x] - field->height[wrap_index(y - 1, size) * size + x])
				/ (2.0f * pixel_meters);
			// (down the picture is +y here: the light from up it, -y)
			float nx = -slope_x;
			float ny = -slope_y;
			float length = sqrtf(nx * nx + ny * ny + 1.0f);
			float lambert = fmaxf((nx * light[0] + ny * light[1] + light[2]) / length, 0.0f);
			float shade = 1.0f + amount * (lambert / flat - 1.0f);
			int index = y * size + x;

			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] *= shade;
			}
		}
	}
}

// Lit as a photograph's small things are: from up the picture's left, a
// little above; for a dome's point (from its middle, of its radius)
static float dome_light(float from_x, float from_y)
{
	float across = fminf(from_x * from_x + from_y * from_y, 1.0f);
	float up = sqrtf(1.0f - across);

	return fmaxf(-0.45f * from_x - 0.45f * from_y + 0.77f * up, 0.0f);
}

// A small round thing (a grain, a pebble) laid on the field: an ellipse at
// x, y (pixels), its radii (pixels), turned; its edge smoothed over a pixel;
// its dome rising height (meters) over what is under it (it settles a
// little into it: sink, of its height); its color lit as dome_light says
// (contrast: how much)
static void splat_dome(field_t *field, float x, float y, float radius_x, float radius_y, float angle, float height, float sink,
	const float color[3], float contrast, float roughness)
{
	int size = field->size;
	float reach = fmaxf(radius_x, radius_y) + 1.5f;
	float c = cosf(angle);
	float s = sinf(angle);
	float base = 0.0f;

	// What it lies on: the field's height under its middle
	base = field->height[wrap_index((int)y, size) * size + wrap_index((int)x, size)];
	for (int py = (int)floorf(y - reach); py <= (int)ceilf(y + reach); py++)
	{
		for (int px = (int)floorf(x - reach); px <= (int)ceilf(x + reach); px++)
		{
			float dx = (float)px + 0.5f - x;
			float dy = (float)py + 0.5f - y;
			float along = (dx * c + dy * s) / radius_x;
			float across = (-dx * s + dy * c) / radius_y;
			float distance = sqrtf(along * along + across * across);
			float cover = clamp_unit((1.0f - distance) * fminf(radius_x, radius_y) + 0.5f);
			int index = wrap_index(py, size) * size + wrap_index(px, size);
			float top;
			float lit;

			if (cover <= 0.0f)
			{
				continue;
			}
			// (its own light: the direction in the picture, not turned)
			{
				float fx = dx / fmaxf(radius_x, radius_y);
				float fy = dy / fmaxf(radius_x, radius_y);

				lit = 1.0f - contrast * 0.5f + contrast * dome_light(fx, fy) * 0.75f;
			}
			top = base + height * (sqrtf(fmaxf(1.0f - distance * distance, 0.0f)) - sink);
			field->height[index] = mix_float(field->height[index], fmaxf(field->height[index], top), cover);
			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] = mix_float(field->color[index * 3 + k], color[k] * lit, cover);
			}
			field->roughness[index] = mix_float(field->roughness[index], roughness, cover);
		}
	}
}

// ---------------------------------------------------------------------------
// Writing

static uint8_t to_byte(float value)
{
	float scaled = value * 255.0f + 0.5f;

	return (uint8_t)(scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled));
}

static void write_to_file(void *context, void *data, int size)
{
	fwrite(data, 1, (size_t)size, (FILE *)context);
}

static bool write_png(const char *path, int size, int channels, const uint8_t *pixels)
{
	FILE *file = fopen(path, "wb");
	bool ok;

	if (file == NULL)
	{
		fprintf(stderr, "potop_texture: cannot write '%s'\n", path);
		return false;
	}
	ok = stbi_write_png_to_func(write_to_file, file, size, size, channels, pixels, size * channels) != 0;
	ok = fclose(file) == 0 && ok;
	if (!ok)
	{
		fprintf(stderr, "potop_texture: cannot write '%s'\n", path);
		return false;
	}
	printf("%s\n", path);
	return true;
}

static bool write_field(const field_t *field, const char *data_directory, const char *name, bool write_height, bool write_roughness, bool write_arm)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	uint8_t *pixels = malloc((size_t)size * (size_t)size * 3);
	char path[1024];
	bool ok = true;
	float lowest = 1e9f;
	float highest = -1e9f;

	if (pixels == NULL)
	{
		return false;
	}
	// The color
	for (int i = 0; i < size * size * 3; i++)
	{
		pixels[i] = to_byte(field->color[i]);
	}
	snprintf(path, sizeof(path), "%s/textures/%s.png", data_directory, name);
	ok = ok && write_png(path, size, 3, pixels);
	// The normal map from the height, as steep as the ground really is
	// (OpenGL's: green up the picture); from the height smoothed over a
	// pixel or so (a steep wall a pixel wide would stair-step)
	float *smooth = malloc((size_t)size * (size_t)size * sizeof(float));

	if (smooth == NULL)
	{
		free(pixels);
		return false;
	}
	memcpy(smooth, field->height, (size_t)size * (size_t)size * sizeof(float));
	blur_wrapping(smooth, size, 1);
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float slope_x = (smooth[y * size + wrap_index(x + 1, size)] - smooth[y * size + wrap_index(x - 1, size)])
				/ (2.0f * pixel_meters);
			float slope_down = (smooth[wrap_index(y + 1, size) * size + x] - smooth[wrap_index(y - 1, size) * size + x])
				/ (2.0f * pixel_meters);
			float nx = -slope_x;
			float ny = slope_down;
			float length = sqrtf(nx * nx + ny * ny + 1.0f);
			uint8_t *pixel = &pixels[(y * size + x) * 3];

			pixel[0] = to_byte(nx / length * 0.5f + 0.5f);
			pixel[1] = to_byte(ny / length * 0.5f + 0.5f);
			pixel[2] = to_byte(1.0f / length * 0.5f + 0.5f);
		}
	}
	free(smooth);
	snprintf(path, sizeof(path), "%s/textures/%s_normal.png", data_directory, name);
	ok = ok && write_png(path, size, 3, pixels);
	// The height, stretched to the whole range (the game wants it only for
	// a relief: its normals are in the _normal file)
	for (int i = 0; i < size * size; i++)
	{
		lowest = fminf(lowest, field->height[i]);
		highest = fmaxf(highest, field->height[i]);
	}
	if (write_height)
	{
		for (int i = 0; i < size * size; i++)
		{
			pixels[i] = to_byte((field->height[i] - lowest) / fmaxf(highest - lowest, 1e-6f));
		}
		snprintf(path, sizeof(path), "%s/textures/%s_height.png", data_directory, name);
		ok = ok && write_png(path, size, 1, pixels);
	}
	// The roughness (a relief has none: it is only shape)
	if (write_roughness)
	{
		for (int i = 0; i < size * size; i++)
		{
			pixels[i] = to_byte(field->roughness[i]);
		}
		snprintf(path, sizeof(path), "%s/textures/%s_roughness.png", data_directory, name);
		ok = ok && write_png(path, size, 1, pixels);
	}
	// Occlusion, roughness, metal in one (a model's material wants them so:
	// the occlusion the hollows under their surroundings)
	if (write_arm)
	{
		float *around = malloc((size_t)size * (size_t)size * sizeof(float));
		uint8_t *arm = malloc((size_t)size * (size_t)size * 3);

		if (around != NULL && arm != NULL)
		{
			memcpy(around, field->height, (size_t)size * (size_t)size * sizeof(float));
			blur_wrapping(around, size, 6);
			for (int i = 0; i < size * size; i++)
			{
				float under = fmaxf(around[i] - field->height[i], 0.0f) / 0.012f;

				arm[i * 3 + 0] = to_byte(1.0f - 0.4f * (1.0f - expf(-under)));
				arm[i * 3 + 1] = to_byte(field->roughness[i]);
				arm[i * 3 + 2] = 0;
			}
			snprintf(path, sizeof(path), "%s/textures/%s_arm.png", data_directory, name);
			ok = ok && write_png(path, size, 3, arm);
		}
		free(around);
		free(arm);
	}
	if (field->mask != NULL)
	{
		for (int i = 0; i < size * size; i++)
		{
			pixels[i] = to_byte(field->mask[i]);
		}
		snprintf(path, sizeof(path), "%s/textures/%s_mask.png", data_directory, name);
		ok = ok && write_png(path, size, 1, pixels);
	}
	printf("  %.2f m across, %.2f mm a pixel, its relief %.1f mm\n", (double)field->meters, (double)(pixel_meters * 1000.0f),
		(double)((highest - lowest) * 1000.0f));
	free(pixels);
	return ok;
}

// ---------------------------------------------------------------------------
// Sand

// A ripple's profile across one wavelength (phase 0..1, 0 the crest): a
// long gentle climb on the windward side, a short steep fall in its lee;
// -1 (the trough) .. 1 (the crest), a little rounded at both
static float ripple_profile(float phase)
{
	float lee = 0.3f;

	phase -= floorf(phase);
	if (phase < lee)
	{
		float t = phase / lee;

		return 1.0f - 2.0f * (t * t * (3.0f - 2.0f * t));
	}
	{
		float t = (phase - lee) / (1.0f - lee);

		return -1.0f + 2.0f * powf(t, 1.5f) * (1.5f - 0.5f * t);
	}
}

// One set of ripples at a point: a whole number of wavelengths across and
// down the picture (so they tile), their crests pushed about by noise
// (winding, forking where the push changes fast), how strong they are
// there (they fade out and break off in patches); -1..1
static float ripple_set(float u, float v, int across, int down, float strength_noise, uint32_t seed)
{
	float waves = sqrtf((float)(across * across + down * down));
	int warp_period = (int)fmaxf(roundf(waves / 3.0f), 2.0f);
	float warp = tiling_fractal(u, v, warp_period, 3, 0.5f, seed + 11u) * 1.1f
		+ tiling_fractal(u, v, warp_period * 3, 2, 0.5f, seed + 23u) * 0.35f;
	float phase = (float)across * u + (float)down * v + warp;
	// Along a crest it rises and falls: crests a few wavelengths long, then
	// a gap, then the next (short crested ripples, not endless lines)
	float along = tiling_fractal(u, v, (int)fmaxf(roundf(waves / 1.6f), 2.0f), 2, 0.5f, seed + 37u);
	float strength = smooth_step(-0.45f, 0.35f, along * 0.8f + strength_noise * 0.6f);

	return ripple_profile(phase) * (0.3f + 0.7f * strength);
}

static void make_sand_ripples(const texture_settings_t *settings, field_t *field, float *trough, float *crest)
{
	int size = field->size;
	float waves = field->meters / settings->ripple_spacing;
	// Two sets: the wind's now, from a little off the picture's axes, and
	// an older or a reflected one across it, a little longer, weaker
	float wind = 0.55f + 0.35f * hash_unit(1, 2, settings->seed);
	float second_wind = wind + 1.0f + 0.25f * hash_unit(3, 4, settings->seed);
	int first[2] = { (int)roundf(waves * cosf(wind)), (int)roundf(waves * sinf(wind)) };
	int second[2] = { (int)roundf(waves / 1.3f * cosf(second_wind)), (int)roundf(waves / 1.3f * sinf(second_wind)) };

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			// Where which set wins: patches of a few wavelengths
			float which = tiling_fractal(u, v, (int)fmaxf(roundf(waves / 5.0f), 2.0f), 2, 0.5f, settings->seed + 51u);
			float a = ripple_set(u, v, first[0], first[1], which, settings->seed + 100u);
			float b = ripple_set(u, v, second[0], second[1], -which, settings->seed + 200u) * 0.75f;
			// Both: the higher of the two crests, smoothly (where they cross,
			// lozenges, where one is gone, the other alone)
			float k = 0.35f;
			float blend = clamp_unit(0.5f + 0.5f * (b - a) / k);
			float h = mix_float(a, b, blend) + k * blend * (1.0f - blend);
			int index = y * size + x;

			field->height[index] = h * settings->ripple_height * 0.5f;
			crest[index] = h;
			// The dark heavy grains in the troughs, in streaks
			trough[index] = smooth_step(0.0f, -0.8f, h) * (0.4f + 0.6f * (0.5f + 0.5f * tiling_fractal(u, v, 40, 2, 0.5f,
				settings->seed + 61u)));
		}
	}
}

// A grain's mineral: its color from the sand's own, by how common each is
static void mineral_color(const float sand[3], float pick, float out[3])
{
	static const struct { float share; float tint[3]; float lightness; } minerals[] =
	{
		{ 0.46f, { 1.00f, 1.00f, 1.00f }, 1.00f },    // quartz, the sand's own
		{ 0.18f, { 1.04f, 0.94f, 0.88f }, 0.95f },    // feldspar, pinkish
		{ 0.13f, { 1.03f, 1.02f, 0.99f }, 1.16f },    // clear quartz, pale
		{ 0.09f, { 0.98f, 0.93f, 0.80f }, 0.82f },    // stained, deeper yellow
		{ 0.05f, { 1.00f, 1.00f, 1.03f }, 1.30f },    // shell, white
		{ 0.05f, { 1.10f, 0.86f, 0.72f }, 0.68f },    // iron stained, rusty
		{ 0.04f, { 0.92f, 0.94f, 0.98f }, 0.28f },    // heavy, dark (magnetite, hornblende)
	};
	int count = (int)(sizeof(minerals) / sizeof(minerals[0]));
	float sum = 0.0f;

	for (int m = 0; m < count; m++)
	{
		sum += minerals[m].share;
		if (pick <= sum || m + 1 == count)
		{
			for (int k = 0; k < 3; k++)
			{
				out[k] = sand[k] * minerals[m].tint[k] * minerals[m].lightness;
			}
			return;
		}
	}
}

// The fine grains: cells of a grain's size, each a mineral and a little
// dome, sampled 3 by 3 in each pixel (a grain is about a pixel: sampled
// once each, they would line up with the pixels and show their grid)
static void make_fine_grains(const texture_settings_t *settings, field_t *field, const float *trough, const float *crest)
{
	int size = field->size;
	float grain_meters = settings->grain_millimeters * 0.001f;
	int cells = (int)fmaxf(roundf(field->meters / grain_meters), 8.0f);

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			int index = y * size + x;
			float color[3] = { 0.0f, 0.0f, 0.0f };
			float height = 0.0f;
			float roughness = 0.0f;
			float dark_more = trough != NULL ? trough[index] * 0.08f : 0.0f;

			for (int sy = 0; sy < 3; sy++)
			{
				for (int sx = 0; sx < 3; sx++)
				{
					float u = ((float)x + ((float)sx + 0.5f) / 3.0f) / (float)size;
					float v = ((float)y + ((float)sy + 0.5f) / 3.0f) / (float)size;
					cells_t grain = tiling_cells(u, v, cells, 0.9f, settings->seed + 301u);
					float pick = hash_unit(grain.cell_x, grain.cell_y, settings->seed + 311u);
					float shade = 0.9f + 0.2f * hash_unit(grain.cell_x, grain.cell_y, settings->seed + 313u);
					float from = grain.nearest / 0.62f;
					float edge = clamp_unit((grain.second - grain.nearest) * 3.0f);
					float lit = 0.72f + 0.42f * dome_light(grain.from_x / 0.62f, grain.from_y / 0.62f) * (0.4f + 0.6f * edge);
					float mineral[3];

					if (pick > 1.0f - dark_more)
					{
						pick = 0.999f;
					}
					else
					{
						pick *= 1.0f - dark_more;
					}
					mineral_color(settings->color, pick, mineral);
					for (int k = 0; k < 3; k++)
					{
						color[k] += mineral[k] * shade * lit;
					}
					height += sqrtf(fmaxf(1.0f - from * from, 0.0f)) * (0.3f + 0.7f * edge);
					// (a clear quartz grain glints on top; the rest frosted)
					roughness += hash_unit(grain.cell_x, grain.cell_y, settings->seed + 317u) < 0.12f ? 0.5f + 0.4f * from : 0.92f;
				}
			}
			{
				float u = ((float)x + 0.5f) / (float)size;
				float v = ((float)y + 0.5f) / (float)size;
				// The sand from place to place: a little damper, finer, more
				// stained (mottles of centimeters to half a meter), paler on
				// the crests (the wind sorts the fine, pale grains up there)
				float mottle = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f), 5, 0.55f, settings->seed + 321u);
				float warm = 0.035f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.8f), 2.0f), 2, 0.5f, settings->seed + 323u);
				float tone = (1.0f + 0.06f * mottle) * (crest != NULL ? 1.0f + 0.08f * crest[index] : 1.0f);
				// The fine grains heaped a little in small mounds between
				float mounds = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.015f), 4.0f), 3, 0.5f, settings->seed + 331u);

				field->color[index * 3 + 0] = color[0] / 9.0f * tone * (1.0f + warm);
				field->color[index * 3 + 1] = color[1] / 9.0f * tone;
				field->color[index * 3 + 2] = color[2] / 9.0f * tone * (1.0f - warm);
				field->height[index] += (height / 9.0f) * grain_meters * 0.4f + mounds * grain_meters * 1.0f;
				field->roughness[index] = roughness / 9.0f;
			}
		}
	}
}

// The coarse grains and the bits of shell: laid on it one by one, a few
// pixels across, lit each
static void make_coarse_grains(const texture_settings_t *settings, field_t *field, const float *trough)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	float grain_pixels = settings->grain_millimeters * 0.001f / pixel_meters;
	// About one in every 7 by 7 grains' room a coarse one (of 1.5 to 4
	// grains), one in 60 by 60 a bit of shell
	int coarse_count = (int)((float)size * (float)size / (grain_pixels * grain_pixels * 49.0f));
	int shell_count = (int)((float)size * (float)size / (grain_pixels * grain_pixels * 3600.0f));

	for (int i = 0; i < coarse_count; i++)
	{
		uint32_t seed = settings->seed + 1000u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float big = 1.5f + 2.5f * powf(hash_unit(i, 2, seed), 2.5f);
		float radius = big * grain_pixels * 0.5f;
		float squash = 0.65f + 0.35f * hash_unit(i, 3, seed);
		float angle = hash_unit(i, 4, seed) * PI_F;
		int index = wrap_index((int)y, size) * size + wrap_index((int)x, size);
		float pick = hash_unit(i, 5, seed);
		float color[3];

		// (more dark ones in the troughs)
		if (trough != NULL && hash_unit(i, 6, seed) < trough[index] * 0.25f)
		{
			pick = 0.999f;
		}
		mineral_color(settings->color, pick, color);
		for (int k = 0; k < 3; k++)
		{
			color[k] *= 0.9f + 0.2f * hash_unit(i, 7 + k, seed);
		}
		splat_dome(field, x, y, radius, radius * squash, angle, radius * pixel_meters * 0.9f, 0.35f, color, 0.9f,
			0.6f + 0.25f * hash_unit(i, 11, seed));
	}
	for (int i = 0; i < shell_count; i++)
	{
		uint32_t seed = settings->seed + 2000u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float length = (2.0f + 4.0f * hash_unit(i, 2, seed)) * grain_pixels;
		float angle = hash_unit(i, 3, seed) * PI_F;
		float tint = hash_unit(i, 4, seed);
		float color[3] =
		{
			fminf(settings->color[0] * 1.32f, 0.97f),
			fminf(settings->color[1] * (1.30f + 0.05f * tint), 0.95f),
			fminf(settings->color[2] * (1.42f + 0.25f * tint), 0.93f),
		};

		// Flat, thin, lying down: half in the sand
		splat_dome(field, x, y, length * 0.5f, length * (0.18f + 0.2f * hash_unit(i, 5, seed)), angle, length * pixel_meters * 0.12f, 0.5f,
			color, 0.5f, 0.35f);
	}
}

static bool make_sand(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float *trough = calloc((size_t)size * (size_t)size, sizeof(float));
	float *crest = calloc((size_t)size * (size_t)size, sizeof(float));
	bool ripples = settings->ripple_spacing > 0.0f;

	if (trough == NULL || crest == NULL)
	{
		free(trough);
		free(crest);
		return false;
	}
	if (ripples)
	{
		make_sand_ripples(settings, field, trough, crest);
	}
	make_fine_grains(settings, field, ripples ? trough : NULL, ripples ? crest : NULL);
	make_coarse_grains(settings, field, ripples ? trough : NULL);
	shade_hollows(field, 2, settings->grain_millimeters * 0.001f * 0.4f, 0.35f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	free(trough);
	free(crest);
	return true;
}

// ---------------------------------------------------------------------------
// Clay: dry, cracked into plates

static bool make_clay(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int big_cells = (int)fmaxf(roundf(field->meters / settings->plate_meters), 2.0f);
	int small_cells = big_cells * 3;
	int pit_cells = (int)fmaxf(roundf(field->meters / 0.012f), 8.0f);

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			// The cracks wander a little at their own scale and are ragged at
			// a few millimeters (not so far that the cells fold over)
			float wander = 0.12f / (float)big_cells;
			float rag = 0.0025f / field->meters;
			int rag_period = (int)fmaxf(roundf(field->meters / 0.01f), 8.0f);
			float warp_u = u + wander * tiling_fractal(u, v, big_cells * 2, 2, 0.5f, settings->seed + 11u)
				+ rag * tiling_fractal(u, v, rag_period, 2, 0.5f, settings->seed + 17u);
			float warp_v = v + wander * tiling_fractal(u, v, big_cells * 2, 2, 0.5f, settings->seed + 13u)
				+ rag * tiling_fractal(u, v, rag_period, 2, 0.5f, settings->seed + 19u);
			int plate_x;
			int plate_y;
			int piece_x;
			int piece_y;
			// How far from a big crack and from a fine one (meters)
			float big_edge = tiling_cell_edge(warp_u, warp_v, big_cells, 0.9f, settings->seed + 21u, &plate_x, &plate_y)
				* field->meters / (float)big_cells;
			float small_edge = tiling_cell_edge(warp_u, warp_v, small_cells, 0.95f, settings->seed + 31u, &piece_x, &piece_y)
				* field->meters / (float)small_cells;
			// The cracks' widths change along them; not every fine crack goes
			// all the way across its plate
			float big_half = 0.5f * 0.006f * (0.5f + (0.5f + 0.5f * tiling_fractal(u, v, big_cells * 3, 2, 0.5f, settings->seed + 41u)));
			float small_there = smooth_step(-0.15f, 0.35f, tiling_fractal(u, v, small_cells, 2, 0.5f, settings->seed + 43u));
			float small_half = 0.5f * 0.0022f * small_there;
			// A V across each crack, as wide as it is and a few pixels more
			// (no cliff a pixel wide: the normals would stair-step)
			float big_v = clamp_unit(1.0f - big_edge / (big_half + pixel_meters * 3.0f));
			float small_v = small_there * clamp_unit(1.0f - small_edge / (small_half + pixel_meters * 2.5f));
			float big_open = 1.0f - smooth_step(big_half - pixel_meters, big_half + pixel_meters, big_edge);
			float small_open = small_there * (1.0f - smooth_step(small_half - pixel_meters, small_half + pixel_meters, small_edge));
			// The finest: hairlines a few centimeters apart, in some places
			float micro_edge = tiling_cell_edge(warp_u, warp_v, small_cells * 4, 0.95f, settings->seed + 33u, NULL, NULL)
				* field->meters / (float)(small_cells * 4);
			float micro_there = smooth_step(0.1f, 0.5f, tiling_fractal(u, v, small_cells * 2, 2, 0.5f, settings->seed + 45u));
			float micro_v = micro_there * clamp_unit(1.0f - micro_edge / (0.0004f + pixel_meters * 1.5f));
			float crack_depth = fmaxf(fmaxf(big_v * big_v * 0.012f, small_v * small_v * 0.004f), micro_v * micro_v * 0.0007f);
			// The crust's own grains: a millimeter or two, each its tone
			cells_t crumb = tiling_cells(u, v, (int)fmaxf(roundf(field->meters / 0.0018f), 16.0f), 0.9f, settings->seed + 75u);
			float crumb_tone = 0.94f + 0.12f * hash_unit(crumb.cell_x, crumb.cell_y, settings->seed + 77u);
			// The plates: their edges curl up as they dry, their middles sag
			// a little; each tilted its own way
			float curl = expf(-big_edge / 0.015f) * 0.002f + expf(-small_edge / 0.008f) * 0.0006f * small_there;
			float plate_tone = 0.94f + 0.12f * hash_unit(plate_x, plate_y, settings->seed + 53u);
			float piece_tone = 1.0f;
			// The crust: fine grit at a millimeter or two, a gentle
			// undulation, now and then a small round pit
			float grit = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.004f), 16.0f), 3, 0.65f, settings->seed + 61u);
			float undulation = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.25f), 2.0f), 4, 0.5f, settings->seed + 65u);
			cells_t pit = tiling_cells(u, v, pit_cells, 0.9f, settings->seed + 67u);
			float pit_there = hash_unit(pit.cell_x, pit.cell_y, settings->seed + 69u) < 0.12f ? 1.0f : 0.0f;
			float pit_depth = pit_there * powf(clamp_unit(1.0f - pit.nearest / 0.22f), 2.0f);
			float height = undulation * 0.003f + curl + grit * 0.0003f - pit_depth * 0.0008f - crack_depth;
			// Its color: the plate's, a little lighter on the dry crust, darker
			// down the cracks' walls, darkest in them
			float mottle = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.6f), 2.0f), 4, 0.55f, settings->seed + 71u);
			// (and small: silt darker, a salty bloom paler, centimeters across)
			float spots = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.03f), 8.0f), 3, 0.55f, settings->seed + 73u);
			float tone = plate_tone * piece_tone * crumb_tone * (1.0f + 0.07f * mottle) * (1.0f + 0.05f * spots) * (1.0f + 0.07f * grit)
				* (1.0f - 0.25f * pit_depth) * (1.0f - 0.18f * micro_v);
			float walls = fmaxf(big_v, small_v * 0.7f);

			tone *= (1.0f - 0.35f * walls * walls) * (1.0f - 0.55f * big_open) * (1.0f - 0.35f * small_open);
			field->height[index] = height;
			field->color[index * 3 + 0] = settings->color[0] * tone * (1.0f - 0.05f * walls);
			field->color[index * 3 + 1] = settings->color[1] * tone;
			field->color[index * 3 + 2] = settings->color[2] * tone * (1.0f + 0.03f * mottle);
			// (the crust a dull matte, the cracks' dust duller, a salty bloom
			// chalky)
			field->roughness[index] = clamp_unit(0.84f + 0.04f * grit + 0.03f * spots + 0.12f * fmaxf(big_open, small_open));
		}
	}
	// Grains and a few small pebbles on it, lighter and darker
	{
		float grain_pixels = 0.0015f / pixel_meters;
		int count = (int)((float)size * (float)size / (grain_pixels * grain_pixels * 160.0f));

		for (int i = 0; i < count; i++)
		{
			uint32_t seed = settings->seed + 3000u;
			float x = hash_unit(i, 0, seed) * (float)size;
			float y = hash_unit(i, 1, seed) * (float)size;
			float big = 1.0f + 3.5f * powf(hash_unit(i, 2, seed), 5.0f);
			float radius = big * grain_pixels * 0.5f;
			float lightness = 0.6f + 0.6f * hash_unit(i, 3, seed);
			float color[3] =
			{
				settings->color[0] * lightness * (0.92f + 0.1f * hash_unit(i, 4, seed)),
				settings->color[1] * lightness,
				settings->color[2] * lightness * (0.95f + 0.15f * hash_unit(i, 5, seed)),
			};

			splat_dome(field, x, y, radius, radius * (0.6f + 0.4f * hash_unit(i, 6, seed)), hash_unit(i, 7, seed) * PI_F,
				radius * pixel_meters * 0.6f, 0.4f, color, 0.45f, 0.8f);
		}
	}
	shade_hollows(field, 3, 0.002f, 0.4f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------
// Granite: a weathered face of crystalline stone

// Facets blended: each cell a plane of its own tilt through its middle,
// the planes weighed by how near their middles are (sharp: how narrow the
// creases between them); meters, and the nearest facet's cell
static float smooth_facets(float u, float v, int cells, float tilt, float sharp, float meters, uint32_t seed, int *cell_x_out, int *cell_y_out)
{
	float cx = u * (float)cells;
	float cy = v * (float)cells;
	int ix = (int)floorf(cx);
	int iy = (int)floorf(cy);
	float sum = 0.0f;
	float weights = 0.0f;
	float nearest = 1e9f;

	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int gx = wrap_index(ix + dx, cells);
			int gy = wrap_index(iy + dy, cells);
			float px = (float)(ix + dx) + 0.5f + (hash_unit(gx, gy, seed) - 0.5f) * 0.9f;
			float py = (float)(iy + dy) + 0.5f + (hash_unit(gx, gy, seed + 1u) - 0.5f) * 0.9f;
			float ox = cx - px;
			float oy = cy - py;
			float distance_squared = ox * ox + oy * oy;
			float slope_x = (hash_unit(gx, gy, seed + 2u) - 0.5f) * tilt;
			float slope_y = (hash_unit(gx, gy, seed + 3u) - 0.5f) * tilt;
			float offset = (hash_unit(gx, gy, seed + 4u) - 0.5f) * tilt * 0.3f;
			float weight = expf(-sharp * distance_squared);

			sum += weight * ((slope_x * ox + slope_y * oy) + offset);
			weights += weight;
			if (distance_squared < nearest)
			{
				nearest = distance_squared;
				*cell_x_out = gx;
				*cell_y_out = gy;
			}
		}
	}
	return sum / fmaxf(weights, 1e-12f) * meters / (float)cells;
}

static bool make_granite(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int block_cells = (int)fmaxf(roundf(field->meters / 1.6f), 2.0f);
	int crystal_cells = (int)fmaxf(roundf(field->meters / 0.007f), 16.0f);
	int lichen_cells = (int)fmaxf(roundf(field->meters / 0.2f), 4.0f);
	float lichen_amount = settings->moss >= 0.0f ? settings->moss : 0.25f;
	float *crevice = calloc((size_t)size * (size_t)size, sizeof(float));

	if (crevice == NULL)
	{
		return false;
	}
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			// Joints: few, far apart, wandering; the blocks between them
			// worn round (granite weathers into domes and corestones)
			float warp_u = u + 0.3f / (float)block_cells * tiling_fractal(u, v, block_cells * 2, 3, 0.5f, settings->seed + 11u);
			float warp_v = v + 0.3f / (float)block_cells * tiling_fractal(u, v, block_cells * 2, 3, 0.5f, settings->seed + 13u);
			int block_x;
			int block_y;
			float joint = tiling_cell_edge(warp_u, warp_v, block_cells, 0.95f, settings->seed + 21u, &block_x, &block_y)
				* field->meters / (float)block_cells
				+ 0.01f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.06f), 8.0f), 3, 0.55f, settings->seed + 23u);
			float gap = 0.006f;
			float out = clamp_unit((joint - gap * 0.5f) / 0.18f);
			float dome = sqrtf(fmaxf(1.0f - (1.0f - out) * (1.0f - out), 0.0f));
			float into = 1.0f - clamp_unit(joint / (gap * 0.5f));
			// Sheets peeling off its skin (exfoliation): shallow steps
			// along the dome, here and there
			float sheet = smooth_step(0.45f, 0.5f, 0.5f + 0.5f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.6f), 2.0f), 3, 0.5f,
				settings->seed + 25u)) * 0.006f;
			float swell = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.5f), 2.0f), 4, 0.5f, settings->seed + 27u);
			float rough = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.03f), 8.0f), 4, 0.6f, settings->seed + 29u);
			// Its crystals: feldspar (pale, pinkish), quartz (gray, glassy),
			// mica and hornblende (black); sampled 3 by 3 (no pixel grid),
			// each a little pit or bump (the surface crumbling grain by grain)
			float crystal[3] = { 0.0f, 0.0f, 0.0f };
			float bumps = 0.0f;
			float glints = 0.0f;

			for (int sy = 0; sy < 3; sy++)
			{
				for (int sx = 0; sx < 3; sx++)
				{
					float su = ((float)x + ((float)sx + 0.5f) / 3.0f) / (float)size;
					float sv = ((float)y + ((float)sy + 0.5f) / 3.0f) / (float)size;
					cells_t grain = tiling_cells(su, sv, crystal_cells, 0.95f, settings->seed + 31u);
					float pick = hash_unit(grain.cell_x, grain.cell_y, settings->seed + 33u);
					float shade = 0.92f + 0.16f * hash_unit(grain.cell_x, grain.cell_y, settings->seed + 35u);
					float mineral[3];

					if (pick < 0.1f)
					{
						mineral[0] = 0.25f; mineral[1] = 0.26f; mineral[2] = 0.28f;
					}
					else if (pick < 0.4f)
					{
						mineral[0] = 0.82f; mineral[1] = 0.83f; mineral[2] = 0.86f;
					}
					else if (pick < 0.75f)
					{
						mineral[0] = 1.12f; mineral[1] = 1.06f; mineral[2] = 1.02f;
					}
					else
					{
						mineral[0] = 1.16f; mineral[1] = 1.0f; mineral[2] = 0.95f;
					}
					for (int k = 0; k < 3; k++)
					{
						crystal[k] += mineral[k] * shade;
					}
					bumps += (pick < 0.4f ? -1.0f : 1.0f) * sqrtf(fmaxf(1.0f - grain.nearest * grain.nearest * 2.0f, 0.0f));
					// (mica's flakes and glassy quartz smoother than the
					// weathered, chalky feldspar)
					glints += pick < 0.1f ? 0.4f : (pick < 0.4f ? 0.55f : 0.82f);
				}
			}
			{
				float height = swell * 0.02f + (dome - 1.0f) * 0.12f - 0.04f * into * into + sheet * dome + rough * 0.003f
					+ bumps / 9.0f * 0.0006f + (hash_unit(block_x, block_y, settings->seed + 37u) - 0.5f) * 0.03f;
				float block_tone = 0.93f + 0.14f * hash_unit(block_x, block_y, settings->seed + 39u);
				float mottle = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f), 4, 0.55f, settings->seed + 41u);
				// Stains down from the joints, the weather's
				float stain = smooth_step(0.0f, 0.5f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.8f), 2.0f), 3, 0.5f,
					settings->seed + 43u)) * (1.0f - out);
				float dark = 0.55f + 0.45f * smooth_step(0.0f, gap + 0.01f, joint);

				field->height[index] = height;
				field->roughness[index] = clamp_unit(glints / 9.0f + 0.06f * rough + 0.15f * stain + 0.2f * into);
				crevice[index] = (1.0f - out) * 0.8f;
				for (int k = 0; k < 3; k++)
				{
					field->color[index * 3 + k] = settings->color[k] * mix_float(crystal[k] / 9.0f, 0.95f, 0.2f) * block_tone * (1.0f + 0.08f * mottle) * dark
						* (1.0f - 0.18f * stain * (k == 2 ? 1.3f : 1.0f));
				}
			}
		}
	}
	shade_hollows(field, 3, 0.01f, 0.35f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	// (none asked for: none)
	if (lichen_amount <= 0.0f)
	{
		free(crevice);
		return true;
	}
	// Lichen: colonies of irregular lobed patches, flat, their edges a
	// little paler and raised, spotted inside; gray-green mostly, a few
	// yellow-orange ones; more where it stays damp (by the joints)
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			float lobes = tiling_fractal(u, v, lichen_cells, 4, 0.55f, settings->seed + 51u)
				+ 0.25f * tiling_fractal(u, v, lichen_cells * 6, 2, 0.5f, settings->seed + 53u);
			float where = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.8f), 2.0f), 2, 0.5f, settings->seed + 55u) * 0.3f
				+ crevice[index] * 0.08f;
			float threshold = 0.42f - lichen_amount * 0.8f - where;
			float cover = smooth_step(threshold, threshold + 0.03f, lobes);
			float rim = smooth_step(threshold + 0.1f, threshold + 0.02f, lobes) * cover;
			float orange_there = smooth_step(0.35f, 0.42f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.25f), 4.0f), 2, 0.5f,
				settings->seed + 57u));
			float spots = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.004f), 16.0f), 2, 0.6f, settings->seed + 59u);
			float gray_green[3] = { 0.60f, 0.64f, 0.52f };
			float orange[3] = { 0.80f, 0.56f, 0.20f };

			if (cover <= 0.0f)
			{
				continue;
			}
			field->height[index] += cover * 0.0006f + rim * 0.0006f + spots * 0.0002f * cover;
			for (int k = 0; k < 3; k++)
			{
				float color = mix_float(gray_green[k], orange[k], orange_there) * (0.88f + 0.18f * spots) * (1.0f + 0.12f * rim);

				field->color[index * 3 + k] = mix_float(field->color[index * 3 + k], color, cover * 0.85f);
			}
			field->roughness[index] = mix_float(field->roughness[index], 0.95f, cover);
		}
	}
	(void)pixel_meters;
	free(crevice);
	return true;
}

// ---------------------------------------------------------------------------
// A dry stone wall's face (a wooden church's footing): field stones of
// every size laid without mortar, the big ones first, smaller ones in the
// room left between them, the smallest wedged in the last gaps; no
// courses. Two kinds of them: river stones (rounded, smooth, gray to
// blue-gray, finely speckled) and limestone (blocky, its outline broken,
// beige, pitted, rough); each its own tone, some with lichen; earth in the
// joints, darker, a little moss in it. The wall's face is the picture,
// +y down it as the wall goes down

#define DRYSTONE_MAX 6000
#define DRYSTONE_BUCKETS 64
#define DRYSTONE_BUCKET_MAX 96

typedef struct
{
	float x;                           // its middle (meters across the picture, down it)
	float y;
	float radius_x;                    // its half sizes (meters), along its own axes
	float radius_y;
	float angle;                       // its long axis' turn from across the picture
	float height;                      // how far its middle stands out of the earth (meters)
	int limestone;                     // 1 limestone, 0 a river stone
	float exponent;                    // its outline: 2 an ellipse .. higher, blockier
	float wobble[3];                   // its outline's waves (3, 5, 7 times round)
	float phase[3];
	int corners;                       // limestone: its outline a broken polygon of these corners (0: smooth)
	float corner_angle[9];             // round it (radians, rising)
	float corner_radius[9];            // and how far out (of its half sizes)
	float tone;                        // its color's lightness, of its kind's
	float warmth;                      // its color's shift, cool (-) .. warm (+)
	float lichen;                      // how much lichen (0 .. 1)
	float tilt_x;                      // its face's lean (meters per meter)
	float tilt_y;
	uint32_t seed;
} drystone_t;

typedef struct
{
	drystone_t stones[DRYSTONE_MAX];
	int count;
	// Which stones reach into each bucket of the picture (64 by 64, wrapping)
	uint16_t buckets[DRYSTONE_BUCKETS][DRYSTONE_BUCKETS][DRYSTONE_BUCKET_MAX];
	uint8_t bucket_counts[DRYSTONE_BUCKETS][DRYSTONE_BUCKETS];
} drystone_wall_t;

// The shortest way from a to b round a period
static float wrap_difference(float a, float b, float period)
{
	float d = fmodf(b - a, period);

	if (d > period * 0.5f)
	{
		d -= period;
	}
	else if (d < -period * 0.5f)
	{
		d += period;
	}
	return d;
}

// How far a point is inside a stone's outline: 0 on it, 1 at its middle,
// below 0 out of it; dx, dy from its middle (meters)
static float drystone_inside(const drystone_t *stone, float dx, float dy)
{
	float c = cosf(stone->angle);
	float s = sinf(stone->angle);
	float lx = (dx * c + dy * s) / stone->radius_x;
	float ly = (-dx * s + dy * c) / stone->radius_y;
	float theta = atan2f(ly, lx);
	float wobble = stone->wobble[0] * sinf(3.0f * theta + stone->phase[0]) + stone->wobble[1] * sinf(5.0f * theta + stone->phase[1])
		+ stone->wobble[2] * sinf(7.0f * theta + stone->phase[2]);
	float d = powf(powf(fabsf(lx), stone->exponent) + powf(fabsf(ly), stone->exponent), 1.0f / stone->exponent);

	// A broken stone: straight edges between its corners (how far out the
	// edge is along this direction, by the line between the two corners
	// either side of it)
	if (stone->corners >= 3)
	{
		float angle = theta < stone->corner_angle[0] ? theta + 2.0f * PI_F : theta;
		int k = 0;

		while (k + 1 < stone->corners && angle >= stone->corner_angle[k + 1])
		{
			k++;
		}
		{
			int next = (k + 1) % stone->corners;
			float a0 = stone->corner_angle[k];
			float a1 = next == 0 ? stone->corner_angle[0] + 2.0f * PI_F : stone->corner_angle[next];
			float x0 = cosf(a0) * stone->corner_radius[k];
			float y0 = sinf(a0) * stone->corner_radius[k];
			float x1 = cosf(a1) * stone->corner_radius[next];
			float y1 = sinf(a1) * stone->corner_radius[next];
			float ex = x1 - x0;
			float ey = y1 - y0;
			float dir_x = cosf(theta);
			float dir_y = sinf(theta);
			// (where the ray from the middle crosses the edge)
			float denominator = dir_x * ey - dir_y * ex;
			float reach = fabsf(denominator) > 1e-6f ? (x0 * ey - y0 * ex) / denominator : 1.0f;
			float r = sqrtf(lx * lx + ly * ly);

			return 1.0f - r / fmaxf(reach * (1.0f + wobble * 0.4f), 0.05f);
		}
	}
	return 1.0f - d / (1.0f + wobble);
}

// Is the room for an ellipse free on the coarse grid (each cell inside it,
// grown by margin meters, unoccupied)? And marking it taken
static bool drystone_room(const uint8_t *taken, int cells, float meters, float x, float y, float radius_x, float radius_y, float angle, float margin,
	bool mark)
{
	float cell = meters / (float)cells;
	float reach = fmaxf(radius_x, radius_y) + margin;
	int x0 = (int)floorf((x - reach) / cell);
	int x1 = (int)ceilf((x + reach) / cell);
	int y0 = (int)floorf((y - reach) / cell);
	int y1 = (int)ceilf((y + reach) / cell);
	float c = cosf(angle);
	float s = sinf(angle);
	float rx = radius_x + margin;
	float ry = radius_y + margin;

	for (int gy = y0; gy <= y1; gy++)
	{
		for (int gx = x0; gx <= x1; gx++)
		{
			float dx = ((float)gx + 0.5f) * cell - x;
			float dy = ((float)gy + 0.5f) * cell - y;
			float lx = (dx * c + dy * s) / rx;
			float ly = (-dx * s + dy * c) / ry;
			int index = wrap_index(gy, cells) * cells + wrap_index(gx, cells);

			if (lx * lx + ly * ly > 1.0f)
			{
				continue;
			}
			if (mark)
			{
				((uint8_t *)taken)[index] = 1;
			}
			else if (taken[index])
			{
				return false;
			}
		}
	}
	return true;
}

// The next of a run of random numbers, 0..1
static float drystone_next(uint32_t *draw)
{
	*draw = hash_u32(*draw + 0x9E3779B9u);
	return (float)(*draw & 0xFFFFFFu) / 16777215.0f;
}

// The stones, laid: in four passes from the biggest down, each stone
// placed at random where it fits
static void drystone_lay(drystone_wall_t *wall, float meters, uint32_t seed)
{
	const int cells = 512;
	uint8_t *taken = calloc((size_t)cells * (size_t)cells, 1);
	// Each pass: its stones' half length along (meters), how many tries
	static const struct { float low; float high; int tries; } passes[] =
	{
		{ 0.13f, 0.2f, 12000 },
		{ 0.09f, 0.14f, 25000 },
		{ 0.055f, 0.09f, 40000 },
		{ 0.032f, 0.055f, 50000 },
		{ 0.018f, 0.032f, 60000 },
		{ 0.01f, 0.018f, 40000 },
	};
	uint32_t draw = seed * 7919u + 1u;

	wall->count = 0;
	if (taken == NULL)
	{
		return;
	}
	for (size_t p = 0; p < sizeof(passes) / sizeof(passes[0]); p++)
	{
		for (int t = 0; t < passes[p].tries && wall->count < DRYSTONE_MAX; t++)
		{
			drystone_t stone;
			float r;

			stone.x = drystone_next(&draw) * meters;
			stone.y = drystone_next(&draw) * meters;
			r = passes[p].low + (passes[p].high - passes[p].low) * drystone_next(&draw);
			stone.radius_x = r;
			// (laid on their flat: longer across than up, mostly)
			stone.radius_y = r * (0.62f + 0.36f * drystone_next(&draw));
			// (some long flat slabs among them, laid on their bed)
			if (p < 2 && drystone_next(&draw) < 0.1f)
			{
				stone.radius_x = r * 1.2f;
				stone.radius_y = r * (0.38f + 0.1f * drystone_next(&draw));
			}
			stone.angle = (drystone_next(&draw) - 0.5f) * (p >= 3 ? 1.6f : 0.45f);
			// (tested a little smaller than it is: the stones pressed
			// together, the joints between them narrow)
			if (!drystone_room(taken, cells, meters, stone.x, stone.y, stone.radius_x * 0.9f, stone.radius_y * 0.9f, stone.angle, 0.0f, false))
			{
				continue;
			}
			stone.limestone = drystone_next(&draw) < 0.72f ? 1 : 0;
			stone.exponent = stone.limestone ? 2.6f + 1.4f * drystone_next(&draw) : 2.0f + 0.3f * drystone_next(&draw);
			for (int k = 0; k < 3; k++)
			{
				float strength = stone.limestone ? 0.14f : 0.07f;

				stone.wobble[k] = strength * drystone_next(&draw) / (float)(k + 1);
				stone.phase[k] = drystone_next(&draw) * 2.0f * PI_F;
			}
			stone.corners = 0;
			if (stone.limestone)
			{
				float start = drystone_next(&draw) * 2.0f * PI_F - PI_F;
				float total = 0.0f;
				float steps[9];

				stone.corners = 5 + (int)(drystone_next(&draw) * 4.0f);
				for (int k = 0; k < stone.corners; k++)
				{
					steps[k] = 0.55f + drystone_next(&draw);
					total += steps[k];
				}
				for (int k = 0; k < stone.corners; k++)
				{
					stone.corner_angle[k] = start;
					// (out past its ellipse a little: its edges between the
					// corners cut back in)
					stone.corner_radius[k] = (0.95f + 0.18f * drystone_next(&draw)) / cosf(PI_F / (float)stone.corners);
					start += steps[k] / total * 2.0f * PI_F;
				}
			}
			stone.height = fminf(fmaxf(fminf(stone.radius_x, stone.radius_y) * (0.4f + 0.3f * drystone_next(&draw)), 0.006f), 0.045f);
			stone.tone = 0.7f + 0.55f * drystone_next(&draw);
			stone.warmth = drystone_next(&draw) - 0.5f;
			stone.lichen = drystone_next(&draw) < 0.8f ? 0.15f + 0.3f * drystone_next(&draw) : 0.0f;
			stone.tilt_x = (drystone_next(&draw) - 0.5f) * 0.12f;
			stone.tilt_y = (drystone_next(&draw) - 0.5f) * 0.12f - 0.03f;
			stone.seed = hash_u32(draw);
			drystone_room(taken, cells, meters, stone.x, stone.y, stone.radius_x * 0.93f, stone.radius_y * 0.93f, stone.angle, 0.0f, true);
			wall->stones[wall->count++] = stone;
		}
	}
	free(taken);
	// Into the buckets: each over the buckets its box reaches
	memset(wall->bucket_counts, 0, sizeof(wall->bucket_counts));
	for (int i = 0; i < wall->count; i++)
	{
		const drystone_t *stone = &wall->stones[i];
		float bucket = meters / (float)DRYSTONE_BUCKETS;
		float reach = fmaxf(stone->radius_x, stone->radius_y) * 1.25f;
		int x0 = (int)floorf((stone->x - reach) / bucket);
		int x1 = (int)floorf((stone->x + reach) / bucket);
		int y0 = (int)floorf((stone->y - reach) / bucket);
		int y1 = (int)floorf((stone->y + reach) / bucket);

		for (int by = y0; by <= y1; by++)
		{
			for (int bx = x0; bx <= x1; bx++)
			{
				int wx = wrap_index(bx, DRYSTONE_BUCKETS);
				int wy = wrap_index(by, DRYSTONE_BUCKETS);

				if (wall->bucket_counts[wy][wx] < DRYSTONE_BUCKET_MAX)
				{
					wall->buckets[wy][wx][wall->bucket_counts[wy][wx]++] = (uint16_t)i;
				}
			}
		}
	}
}

static bool make_drystone(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float meters = field->meters;
	float pixel_meters = meters / (float)size;
	drystone_wall_t *wall = malloc(sizeof(drystone_wall_t));
	int speckle_cells = (int)fmaxf(roundf(meters / 0.0022f), 32.0f);

	if (wall == NULL)
	{
		return false;
	}
	drystone_lay(wall, meters, settings->seed);
	printf("  %d stones\n", wall->count);
#pragma omp parallel for schedule(dynamic, 8)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			float px = u * meters;
			float py = v * meters;
			int index = y * size + x;
			int bx = wrap_index((int)floorf(px / (meters / (float)DRYSTONE_BUCKETS)), DRYSTONE_BUCKETS);
			int by = wrap_index((int)floorf(py / (meters / (float)DRYSTONE_BUCKETS)), DRYSTONE_BUCKETS);
			// (its outline a little ragged: the point looked up moved by a
			// fine noise)
			float jitter_x = 0.0025f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.05f), 8.0f), 2, 0.45f, settings->seed + 3u);
			float jitter_y = 0.0025f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.05f), 8.0f), 2, 0.45f, settings->seed + 5u);
			int best = -1;
			float best_in = -1e9f;
			float second_in = -1e9f;
			bool pressed = false;
			float own_in = 0.0f;
			float best_dx = 0.0f;
			float best_dy = 0.0f;
			// The earth between them: dark, crumbly, a few grits
			float crumbs = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.02f), 8.0f), 4, 0.55f, settings->seed + 7u);
			float earth_height = -0.004f + 0.003f * crumbs;
			float height = earth_height;
			// (not earth: grit and chips of the same stone packed in, gray)
			float color[3] = { 0.36f, 0.325f, 0.27f };
			float roughness = 0.95f;
			float rim = 0.0f;

			{
				cells_t chip = tiling_cells(u, v, (int)fmaxf(roundf(meters / 0.006f), 32.0f), 0.9f, settings->seed + 9u);
				float chip_tone = hash_unit(chip.cell_x, chip.cell_y, settings->seed + 10u);
				float chip_dome = sqrtf(fmaxf(1.0f - chip.nearest * chip.nearest * 2.2f, 0.0f));

				for (int k = 0; k < 3; k++)
				{
					color[k] *= (0.7f + 0.5f * chip_tone) * (0.75f + 0.35f * chip_dome);
				}
				earth_height += 0.0015f * chip_dome;
			}
			for (int b = 0; b < wall->bucket_counts[by][bx]; b++)
			{
				const drystone_t *stone = &wall->stones[wall->buckets[by][bx][b]];
				float dx = wrap_difference(stone->x, px + jitter_x, meters);
				float dy = wrap_difference(stone->y, py + jitter_y, meters);
				// (into meters from its edge, about: of its smaller half size)
				float in = drystone_inside(stone, dx, dy) * fminf(stone->radius_x, stone->radius_y);

				// (limestone's edge chipped: bites out of it, of a few
				// centimeters, here and there round it)
				if (stone->limestone && in < 0.03f && in > -0.01f)
				{
					float chips = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.03f), 16.0f), 3, 0.55f, stone->seed + 41u);

					in -= 0.012f * smooth_step(0.15f, 0.5f, chips);
				}

				if (in > best_in)
				{
					second_in = best_in;
					best_in = in;
					best = wall->buckets[by][bx][b];
					best_dx = dx;
					best_dy = dy;
				}
				else if (in > second_in)
				{
					second_in = in;
				}
			}
			// Where two stones press into each other: a narrow joint
			// between them (their meeting line, a few millimeters in from
			// both), not one running into the other
			if (second_in > 0.0f)
			{
				float seam = (best_in - second_in) * 0.5f - 0.0012f;

				own_in = best_in;
				if (seam < best_in)
				{
					best_in = seam;
					pressed = true;
				}
			}
			if (best >= 0 && best_in > -0.004f)
			{
				const drystone_t *stone = &wall->stones[best];
				float small = fminf(stone->radius_x, stone->radius_y);
				float t = clamp_unit(best_in / small);
				float profile;
				float stone_height;
				float surface;
				float pits = 0.0f;
				float stone_color[3];

				// Its face: a river stone's a round dome; limestone's flatter,
				// its edges broken off steeper, its face in facets
				if (stone->limestone)
				{
					profile = smooth_step(0.0f, 0.45f, t);
					profile = profile * (0.85f + 0.15f * t);
					{
						int facet_x;
						int facet_y;

						// (broken in facets, their edges soft: no lines)
						surface = smooth_facets(u, v, (int)fmaxf(roundf(meters / 0.07f), 8.0f), 0.5f, 6.0f, meters, stone->seed, &facet_x, &facet_y);
					}
					// Its pores: holes eaten into it, of every size, in clusters
					{
						float holes = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.03f), 16.0f), 3, 0.6f, stone->seed + 9u)
							+ 0.35f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.4f), 4.0f), 2, 0.5f, stone->seed + 11u);

						pits = smooth_step(0.42f, 0.6f, holes);
					}
					surface += 0.005f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.025f), 16.0f), 5, 0.62f, stone->seed + 13u) - 0.005f * pits;
				}
				else
				{
					profile = sqrtf(fmaxf(1.0f - (1.0f - t) * (1.0f - t), 0.0f));
					surface = 0.0022f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.03f), 8.0f), 4, 0.6f, stone->seed + 13u);
				}
				// (it sinks a little into the earth at its edge; its face leans)
				stone_height = stone->height * profile - 0.006f + surface + (stone->tilt_x * best_dx + stone->tilt_y * best_dy) * profile;
				// (a seam where two stones press together: shallow, not a
				// gully down to the grit)
				// (its dome by its own outline, the seam only a narrow groove
				// along where the two meet)
				if (pressed)
				{
					float own_t = clamp_unit(own_in / small);
					float own_profile = stone->limestone ? smooth_step(0.0f, 0.45f, own_t) * (0.85f + 0.15f * own_t)
						: sqrtf(fmaxf(1.0f - (1.0f - own_t) * (1.0f - own_t), 0.0f));
					float groove = 1.0f - smooth_step(0.0f, 0.009f, best_in);

					stone_height = stone->height * own_profile - 0.006f + surface + (stone->tilt_x * best_dx + stone->tilt_y * best_dy) * own_profile
						- 0.009f * groove;
				}
				// (the stone wherever it is, over the grit even at its very
				// edge: the joints only as wide as the gaps between stones)
				height = best_in > 0.0015f ? fmaxf(stone_height, earth_height + 0.0008f) : fmaxf(earth_height, stone_height);
				// Its color: its kind's, its tone and warmth, speckled; its
				// rim, down in the earth, shaded
				if (stone->limestone)
				{
					stone_color[0] = 0.62f;
					stone_color[1] = 0.60f;
					stone_color[2] = 0.56f;
				}
				else
				{
					stone_color[0] = 0.47f;
					stone_color[1] = 0.465f;
					stone_color[2] = 0.44f;
				}
				{
					// Its grains (a river stone's crystals, limestone's grit),
					// its mottle, a quartz vein through some river stones,
					// earth stains on its lower part, the weather's paler top
					cells_t grain = tiling_cells(u, v, speckle_cells, 0.95f, stone->seed + 17u);
					float grain_tone = hash_unit(grain.cell_x, grain.cell_y, stone->seed + 18u);
					float mottle = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.12f), 4.0f), 4, 0.55f, stone->seed + 19u);
					// (a vein: a band straight across the stone, a little
					// wavering, its edges soft)
					float vein_angle = (float)(stone->seed >> 8 & 255u) / 255.0f * PI_F;
					float vein_across = best_dx * cosf(vein_angle) + best_dy * sinf(vein_angle)
						- ((float)(stone->seed >> 16 & 255u) / 255.0f - 0.5f) * fminf(stone->radius_x, stone->radius_y)
						+ 0.004f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.05f), 8.0f), 2, 0.5f, stone->seed + 21u);
					float vein_width = 0.002f + 0.004f * (float)(stone->seed >> 24 & 255u) / 255.0f;
					float vein = !stone->limestone && (stone->seed & 7u) == 0u ? smooth_step(vein_width + 0.0015f, vein_width, fabsf(vein_across)) : 0.0f;
					float down = clamp_unit(best_dy / fmaxf(stone->radius_y, 1e-4f) * 0.5f + 0.5f);
					float stain = smooth_step(0.45f, 1.0f, down) * 0.3f;
					float warm[3] = { 1.0f + 0.05f * stone->warmth, 1.0f, 1.0f - 0.05f * stone->warmth + 0.03f };
					// (weathered patches, darker gray: the rain's, the old
					// lichen's, blotches of every size)
					float weather = smooth_step(-0.05f, 0.4f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.04f), 8.0f), 4, 0.62f, stone->seed + 31u)
						+ 0.3f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.2f), 4.0f), 2, 0.5f, stone->seed + 33u));
					// (speckles: a river stone's dark and pale crystals, small
					// lichen dots)
					cells_t dot = tiling_cells(u, v, (int)fmaxf(roundf(meters / 0.008f), 32.0f), 0.95f, stone->seed + 35u);
					float dot_there = hash_unit(dot.cell_x, dot.cell_y, stone->seed + 36u);
					float dots = dot_there < 0.25f ? smooth_step(0.42f, 0.2f, dot.nearest) : 0.0f;
					float dot_light = hash_unit(dot.cell_x, dot.cell_y, stone->seed + 37u) < 0.55f ? 1.0f : -1.0f;
					float grit = stone->limestone ? 0.32f : 0.4f;
					float crumbly = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.012f), 16.0f), 3, 0.6f, stone->seed + 27u);

					for (int k = 0; k < 3; k++)
					{
						float stain_color = k == 0 ? 0.85f : (k == 1 ? 0.78f : 0.68f);

						stone_color[k] *= stone->tone * warm[k] * (1.0f + 0.22f * mottle)
							* (1.0f - grit * 0.5f + grit * grain_tone) * (1.0f - 0.55f * pits) * (1.0f + (stone->limestone ? 0.4f : 0.25f) * crumbly);
						// (the weathered crust grayer, not only darker)
						stone_color[k] = mix_float(stone_color[k], (k == 0 ? 0.42f : (k == 1 ? 0.415f : 0.40f)) * stone->tone, weather * 0.45f);
						stone_color[k] *= 1.0f - 0.12f * weather;
						stone_color[k] *= 1.0f + 0.22f * dots * dot_light;
						stone_color[k] = mix_float(stone_color[k], 0.78f, vein * 0.55f);
						stone_color[k] *= mix_float(1.0f, stain_color, stain) * (1.0f + 0.08f * (1.0f - down));
					}
				}
				// Lichen on its face, patchy (gray-white, a few yellow-green)
				if (stone->lichen > 0.0f)
				{
					float lobes = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.025f), 16.0f), 4, 0.6f, stone->seed + 23u)
						+ 0.4f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.2f), 4.0f), 2, 0.5f, stone->seed + 25u);
					// (finer on the smooth river stones: a crust of small
					// spots, not patches)
					float fine = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.01f), 32.0f), 3, 0.6f, stone->seed + 24u);
					float cover = smooth_step(0.6f - stone->lichen, 0.66f - stone->lichen, stone->limestone ? lobes : lobes * 0.5f + fine * 0.8f)
						* smooth_step(0.08f, 0.3f, t);
					float yellow = smooth_step(0.45f, 0.6f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.15f), 4.0f), 2, 0.5f, stone->seed + 29u));
					float lichen_color[3] = { mix_float(0.74f, 0.66f, yellow), mix_float(0.74f, 0.64f, yellow), mix_float(0.69f, 0.36f, yellow) };

					for (int k = 0; k < 3; k++)
					{
						stone_color[k] = mix_float(stone_color[k], lichen_color[k], cover * (stone->limestone ? 0.6f : 0.45f));
					}
					height += cover * 0.0005f;
					roughness = mix_float(roughness, 0.92f, cover);
				}
				// (its edge rounding down into the joint, a centimeter or two)
				rim = (1.0f - smooth_step(0.0f, 0.016f, best_in)) * (pressed ? 0.75f : 1.0f);
				if (best_in > 0.0015f || stone_height >= earth_height)
				{
					for (int k = 0; k < 3; k++)
					{
						color[k] = stone_color[k] * (1.0f - 0.25f * rim * rim);
					}
					roughness = stone->limestone ? 0.85f : 0.62f;
				}
			}
			// A little moss in the earth, along the stones
			{
				float moss = smooth_step(0.35f, 0.55f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.1f), 4.0f), 3, 0.5f, settings->seed + 31u));

				if (height <= earth_height + 0.0005f && moss > 0.0f)
				{
					float moss_color[3] = { 0.16f, 0.2f, 0.07f };

					for (int k = 0; k < 3; k++)
					{
						color[k] = mix_float(color[k], moss_color[k] * (0.8f + 0.4f * (crumbs * 0.5f + 0.5f)), moss * 0.6f);
					}
				}
			}
			field->height[index] = height;
			field->roughness[index] = roughness;
			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] = color[k];
			}
		}
	}
	(void)pixel_meters;
	free(wall);
	shade_hollows(field, 3, 0.008f, 0.45f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------
// A coursed stone wall: limestone roughly squared, laid in courses of
// their own heights, blocks of every length along each, here and there two
// thin ones one on the other in a course; the faces hammered rough, their
// edges pitched off round, a little proud of the mortar; lime mortar with
// its grit in the joints, set back. Repeats both ways (the courses fill its
// height exactly, the blocks each course's width)

#define COURSED_MAX_COURSES 48
#define COURSED_MAX_BLOCKS 80

typedef struct
{
	float x0;                          // along (meters, from the course's start; the last ends at the picture's width)
	float x1;
	int split;                         // 0 one stone; 1 two, one on the other, split at split_at (of the course's height)
	float split_at;
	int tall;                          // 1: it stands two courses high (over the next course's stones there)
	uint32_t seed;
} coursed_block_t;

typedef struct
{
	float y0;                          // down the picture (meters)
	float y1;
	int block_count;
	coursed_block_t blocks[COURSED_MAX_BLOCKS];
} coursed_course_t;

typedef struct
{
	int course_count;
	coursed_course_t courses[COURSED_MAX_COURSES];
} coursed_wall_t;

// Fills a course from x0 to x1 (meters, may run past the picture's width)
// with stones: mostly middling, some long, a few near square, none
// narrower than the course is high; the long ones on an even course may
// stand two courses high
static void coursed_fill(coursed_course_t *course, int course_index, int may_stand_tall, float x0, float x1, float meters, uint32_t *draw)
{
	float h = course->y1 - course->y0;
	float width = x1 - x0;
	float lengths[COURSED_MAX_BLOCKS];
	float sum = 0.0f;
	int n = 0;
	int first = course->block_count;

	if (width < 0.35f * h)
	{
		return;
	}
	if (width < 1.6f * h)
	{
		lengths[n++] = width;
		sum = width;
	}
	else
	{
		while (sum < width && n < COURSED_MAX_BLOCKS - first)
		{
			lengths[n] = drystone_next(draw) < 0.2f ? h * (1.0f + 0.3f * drystone_next(draw))
				: h * (1.3f + 0.6f * drystone_next(draw) * drystone_next(draw) + 0.9f * drystone_next(draw));
			sum += lengths[n];
			n++;
		}
		// (the last one, if it would come out short, given to the one
		// before it)
		if (n > 1 && lengths[n - 1] - (sum - width) < 0.8f * h)
		{
			sum -= lengths[n - 1];
			n--;
		}
	}
	{
		float x = x0;

		for (int i = 0; i < n; i++)
		{
			coursed_block_t *block = &course->blocks[first + i];
			float length = lengths[i] * width / sum;

			// (its start brought round into the picture's width: a
			// stone is looked for a width either side, no further)
			block->x0 = x - meters * floorf(x / meters);
			block->x1 = block->x0 + length;
			x += length;
			block->split = drystone_next(draw) < 0.0f ? 1 : 0;
			block->tall = (may_stand_tall && length > 2.0f * h && length < 3.2f * h && drystone_next(draw) < 0.18f) ? 1 : 0;
			if (block->tall)
			{
				block->split = 0;
			}
			block->split_at = 0.38f + 0.24f * drystone_next(draw);
			block->seed = hash_u32(*draw + (uint32_t)(course_index * 131 + first + i));
		}
	}
	course->block_count = first + n;
}

static void coursed_lay(coursed_wall_t *wall, float meters, float course_height, uint32_t seed)
{
	uint32_t draw = seed * 9176u + 7u;
	float heights[COURSED_MAX_COURSES];
	float total = 0.0f;
	int count = 0;

	while (total < meters && count < COURSED_MAX_COURSES)
	{
		heights[count] = course_height * (0.75f + 0.6f * drystone_next(&draw));
		total += heights[count];
		count++;
	}
	// (scaled to fill the picture's height exactly: it repeats down it)
	{
		float y = 0.0f;

		for (int c = 0; c < count; c++)
		{
			coursed_course_t *course = &wall->courses[c];
			// (a tall stone only on the even courses, not the last: two
			// never stack, and none runs past the picture's bottom)
			int may_stand_tall = c % 2 == 0 && c < count - 1;
			const coursed_course_t *above = c > 0 ? &wall->courses[c - 1] : NULL;
			int talls[COURSED_MAX_BLOCKS];
			int tall_count = 0;

			course->y0 = y;
			y += heights[c] * meters / total;
			course->y1 = c == count - 1 ? meters : y;
			course->block_count = 0;
			for (int b = 0; above != NULL && b < above->block_count; b++)
			{
				if (above->blocks[b].tall)
				{
					talls[tall_count++] = b;
				}
			}
			if (tall_count == 0)
			{
				float start = drystone_next(&draw) * meters;

				coursed_fill(course, c, may_stand_tall, start, start + meters, meters, &draw);
			}
			else
			{
				// (the stones round the tall ones above, which reach down
				// through this course: from the first one's end, round the
				// picture, back to its start)
				float start = above->blocks[talls[0]].x1;
				float at = start;

				for (int t = 1; t <= tall_count; t++)
				{
					const coursed_block_t *tall = &above->blocks[talls[t % tall_count]];
					float tall_x0 = tall->x0;
					float tall_x1 = tall->x1;

					while (tall_x0 < at - 1e-5f)
					{
						tall_x0 += meters;
						tall_x1 += meters;
					}
					coursed_fill(course, c, may_stand_tall, at, tall_x0, meters, &draw);
					at = tall_x1;
				}
			}
		}
	}
	wall->course_count = count;
}

// Signed distance to a rounded box's edge (negative inside), from its
// middle by dx, dy, its half sizes, its corners' radius
static float coursed_box_distance(float dx, float dy, float half_x, float half_y, float radius)
{
	float qx = fabsf(dx) - half_x + radius;
	float qy = fabsf(dy) - half_y + radius;
	float outside = sqrtf(fmaxf(qx, 0.0f) * fmaxf(qx, 0.0f) + fmaxf(qy, 0.0f) * fmaxf(qy, 0.0f));

	return outside + fminf(fmaxf(qx, qy), 0.0f) - radius;
}

// A crumpled surface, piecewise flat: at each point the lowest of the planes
// of the cells round it (each cell a plane through its point, tilted its
// own way), so the facets meet in sharp creases and nothing steps; cells
// across the picture, tilt the planes' slope, its height (meters)
static float crumpled(float u, float v, int cells_x, int cells_y, float tilt, float meters, uint32_t seed)
{
	float cx = u * (float)cells_x;
	float cy = v * (float)cells_y;
	int ix = (int)floorf(cx);
	int iy = (int)floorf(cy);
	float lowest = 1e9f;

	for (int dy = -2; dy <= 2; dy++)
	{
		for (int dx = -2; dx <= 2; dx++)
		{
			int gx = wrap_index(ix + dx, cells_x);
			int gy = wrap_index(iy + dy, cells_y);
			float px = (float)(ix + dx) + 0.5f + (hash_unit(gx, gy, seed) - 0.5f) * 0.8f;
			float py = (float)(iy + dy) + 0.5f + (hash_unit(gx, gy, seed + 1u) - 0.5f) * 0.8f;
			float slope_x = (hash_unit(gx, gy, seed + 2u) - 0.5f) * 2.0f * tilt;
			float slope_y = (hash_unit(gx, gy, seed + 3u) - 0.5f) * 2.0f * tilt;
			// (each plane up from its point: the cell's own the lowest there)
			float d2 = (cx - px) * (cx - px) + (cy - py) * (cy - py);
			float plane = slope_x * (cx - px) + slope_y * (cy - py) + 0.25f * d2;

			lowest = fminf(lowest, plane);
		}
	}
	return lowest * meters / (float)cells_y;
}

static bool make_coursed(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float meters = field->meters;
	coursed_wall_t *wall = malloc(sizeof(coursed_wall_t));
	float joint = 0.011f;

	if (wall == NULL)
	{
		return false;
	}
	coursed_lay(wall, meters, 0.15f, settings->seed);
	printf("  %d courses\n", wall->course_count);
#pragma omp parallel for schedule(dynamic, 8)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			float px = u * meters;
			float py = v * meters;
			int index = y * size + x;
			// (the edges ragged: pitched off by hand, a few millimeters
			// in and out, and here and there a bigger bite)
			float ragged = 0.004f * (tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.025f), 16.0f), 3, 0.6f, settings->seed + 3u) - 0.1f)
				+ 0.008f * smooth_step(0.15f, 0.55f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.09f), 8.0f), 3, 0.55f, settings->seed + 5u));
			// The mortar: lime, its grit and small stones in it, set back
			float grit_noise = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.01f), 32.0f), 3, 0.6f, settings->seed + 7u);
			cells_t grain = tiling_cells(u, v, (int)fmaxf(roundf(meters / 0.006f), 64.0f), 0.95f, settings->seed + 9u);
			float grain_tone = hash_unit(grain.cell_x, grain.cell_y, settings->seed + 10u);
			float grain_dome = sqrtf(fmaxf(1.0f - grain.nearest * grain.nearest * 2.2f, 0.0f));
			// (its gravel: small rounded stones in it, a few millimeters to a
			// centimeter, standing out of it)
			cells_t pebble = tiling_cells(u, v, (int)fmaxf(roundf(meters / 0.01f), 64.0f), 0.95f, settings->seed + 13u);
			float pebble_there = hash_unit(pebble.cell_x, pebble.cell_y, settings->seed + 14u);
			float pebble_size = 0.15f + 0.45f * powf(hash_unit(pebble.cell_x, pebble.cell_y, settings->seed + 15u), 2.0f);
			float pebble_dome = pebble_there < 0.65f ? sqrtf(fmaxf(1.0f - (pebble.nearest / pebble_size) * (pebble.nearest / pebble_size), 0.0f)) : 0.0f;
			float pebble_tone = hash_unit(pebble.cell_x, pebble.cell_y, settings->seed + 16u);
			float mortar_height = -0.0055f + 0.001f * grit_noise + 0.0006f * grain_dome + 0.0012f * pebble_dome * pebble_size * 2.0f;
			float color[3] = { 0.93f, 0.89f, 0.81f };
			float height = mortar_height;
			float roughness = 0.95f;
			int c = 0;

			for (int k = 0; k < 3; k++)
			{
				// (lime: pale, its sand faint; a few gravel stones in it,
				// some dark)
				color[k] *= (0.96f + 0.05f * grain_tone) * (0.98f + 0.03f * grain_dome);
				// (the pebbles: limestone mostly, a few gray)
				if (pebble_dome > 0.0f)
				{
					float pebble_color = pebble_tone < 0.12f ? 0.6f : (0.9f + 0.1f * pebble_tone);

					color[k] = mix_float(color[k], pebble_color * (k == 2 ? 0.95f : 1.0f), 0.85f * smooth_step(0.0f, 0.25f, pebble_dome));
					// (its rim in the lime, a hair darker)
					color[k] *= 1.0f - 0.12f * smooth_step(0.25f, 0.0f, pebble_dome) * (pebble_dome > 0.0f ? 1.0f : 0.0f);
				}
			}
			while (c < wall->course_count - 1 && py >= wall->courses[c].y1)
			{
				c++;
			}
			// Its stone: the block in its course whose span holds it (the
			// course's blocks wrap round the picture's width)
			{
				const coursed_block_t *block = NULL;
				float block_x0 = 0.0f;
				float block_x1 = 0.0f;
				float y0 = wall->courses[c].y0;
				float y1 = wall->courses[c].y1;

				// (the course above first: a tall stone of it reaches down here)
				for (int pass = 0; pass < 2 && block == NULL; pass++)
				{
					int from = pass == 0 ? c - 1 : c;
					const coursed_course_t *course;

					if (from < 0)
					{
						continue;
					}
					course = &wall->courses[from];
					for (int b = 0; b < course->block_count && block == NULL; b++)
					{
						if (pass == 0 && !course->blocks[b].tall)
						{
							continue;
						}
						for (int w = -1; w <= 1; w++)
						{
							float x0 = course->blocks[b].x0 + (float)w * meters;
							float x1 = course->blocks[b].x1 + (float)w * meters;

							if (px >= x0 && px < x1)
							{
								block = &course->blocks[b];
								block_x0 = x0;
								block_x1 = x1;
								y0 = course->y0;
								y1 = course->blocks[b].tall ? wall->courses[from + 1].y1 : course->y1;
								break;
							}
						}
					}
				}
				if (block != NULL)
				{
					float distance;
					float half_x;
					float half_y;
					float middle_x;
					float middle_y;
					uint32_t stone_seed = block->seed;

					// (two thin stones: the upper or the lower)
					if (block->split)
					{
						float at = y0 + (y1 - y0) * block->split_at;

						if (py < at)
						{
							y1 = at;
						}
						else
						{
							y0 = at;
							stone_seed = hash_u32(stone_seed + 1u);
						}
					}
					middle_x = (block_x0 + block_x1) * 0.5f;
					middle_y = (y0 + y1) * 0.5f;
					half_x = (block_x1 - block_x0) * 0.5f - joint * 0.5f;
					half_y = (y1 - y0) * 0.5f - joint * 0.5f;
					{
						// (each its own shape: its corners rounder or squarer,
						// its sides a little out of true)
						float corner = fminf(half_x, half_y) * (0.15f + 0.4f * hash_unit((int)(stone_seed & 1023u), 1, stone_seed));
						float lean = (hash_unit((int)(stone_seed & 1023u), 2, stone_seed) - 0.5f) * 0.22f;
						// (not every stone fills its course: some a little
						// short of it, the mortar thicker over or under them;
						// and some narrower at one end, or their top slanting)
						float shrink_y = half_y * 0.05f * powf(hash_unit((int)(stone_seed & 1023u), 3, stone_seed), 2.0f);
						float shift_y = (hash_unit((int)(stone_seed & 1023u), 9, stone_seed) - 0.5f) * shrink_y * 1.6f;
						float shrink_end = (hash_unit((int)(stone_seed & 1023u), 4, stone_seed) - 0.3f) * joint * 1.5f;
						float taper = (hash_unit((int)(stone_seed & 1023u), 10, stone_seed) - 0.5f) * 0.4f;
						float slant = (hash_unit((int)(stone_seed & 1023u), 11, stone_seed) - 0.5f) * 0.16f;
						float dx = px - middle_x + lean * (py - middle_y);
						float dy = py - middle_y - shift_y + slant * (px - middle_x);
						// (kept inside its own place: leaning, slanting or tapering, it
						// gives up as much as it would push into its neighbors)
						float local_half_y = (half_y - shrink_y - 0.35f * fabsf(slant) * fminf(half_x, 3.0f * half_y)) * (1.0f - fabsf(taper) * 0.2f + taper * 0.2f * (px - middle_x) / fmaxf(half_x, 1e-4f));

						distance = coursed_box_distance(dx, dy, half_x - fmaxf(shrink_end, 0.0f) - 0.7f * fabsf(lean) * half_y, fmaxf(local_half_y, half_y * 0.4f), corner) + ragged;
						// (a corner knocked off at a slant, on some: the stone
						// a trapezoid or a pentagon)
						{
							uint32_t cut = stone_seed >> 12;

							if ((cut & 1u) == 0u)
							{
								float sx = (cut & 4u) ? 1.0f : -1.0f;
								float sy = (cut & 8u) ? 1.0f : -1.0f;
								float depth = 0.85f + 0.2f * (float)((cut >> 4) & 15u) / 15.0f;
								float plane = (dx * sx / fmaxf(half_x, 1e-4f) * 0.5f + dy * sy / fmaxf(half_y, 1e-4f)) - depth * 1.2f;

								distance = fmaxf(distance, plane * half_y * 0.7f);
							}
						}
					}
					// (the mortar's shade right under a stone's edge: the
					// stone stands out over it)
					if (distance >= 0.0f && distance < 0.009f)
					{
						// (and here and there a void: the mortar not filled up
						// to the stone, a dark gap in its shadow)
						float gap = smooth_step(0.05f, 0.35f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.05f), 8.0f), 3, 0.55f, settings->seed + 31u))
							* smooth_step(0.009f, 0.002f, distance);
						float shade = (1.0f - 0.12f * fmaxf(1.0f - distance / 0.006f, 0.0f) * fmaxf(1.0f - distance / 0.006f, 0.0f)) * (1.0f - 0.32f * gap);

						height -= 0.0008f * gap;

						for (int k = 0; k < 3; k++)
						{
							color[k] *= shade;
						}
					}
					if (distance < 0.0f)
					{
						float in = -distance;
						float small = fminf(half_x, half_y);
						float t = clamp_unit(in / 0.016f);
						float pitched = sqrtf(fmaxf(1.0f - (1.0f - t) * (1.0f - t), 0.0f));
						// Its face: hammered rough (dents of a few millimeters,
						// in clusters), broken in shallow facets, a little
						// domed and tilted
						int facet_x;
						int facet_y;
						// (crisp: broken planes meeting in sharp creases, as
						// hammered limestone is, at three sizes)
						float facets = 0.4f * smooth_facets(u, v, (int)fmaxf(roundf(meters / 0.06f), 8.0f), 0.5f, 10.0f, meters, stone_seed, &facet_x, &facet_y)
							+ 0.35f * smooth_facets(u, v, (int)fmaxf(roundf(meters / 0.015f), 16.0f), 0.6f, 20.0f, meters, stone_seed + 5u, &facet_x, &facet_y)
							+ 0.45f * smooth_facets(u, v, (int)fmaxf(roundf(meters / 0.006f), 32.0f), 0.7f, 20.0f, meters, stone_seed + 6u, &facet_x, &facet_y);
						float hammered = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.009f), 32.0f), 4, 0.62f, stone_seed + 11u);
						float tilt = (hash_unit((int)(stone_seed & 1023u), 5, stone_seed) - 0.5f) * 0.03f * (px - middle_x)
							+ (hash_unit((int)(stone_seed & 1023u), 6, stone_seed) - 0.5f) * 0.03f * (py - middle_y);
						float pits = smooth_step(0.5f, 0.68f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.02f), 16.0f), 3, 0.6f, stone_seed + 13u));
						// (the facets mostly by the edges, where it was pitched off; its
						// middle the bed's flatter face)
						// (and conchoidal scars: shallow scoops broken off its face)
						cells_t scar = tiling_cells(u, v, (int)fmaxf(roundf(meters / 0.035f), 16.0f), 0.9f, stone_seed + 21u);
						float scar_there = hash_unit(scar.cell_x, scar.cell_y, stone_seed + 22u) < 0.35f ? 1.0f : 0.0f;
						float scoop = scar_there * smooth_step(0.5f, 0.15f, scar.nearest) * (1.0f - scar.nearest * scar.nearest);
						// (the hammered limestone's crumpled skin: sharp ridges
						// and hollows of a few centimeters and of millimeters,
						// lit crisp)
						float ridges = 0.0f;

						{
							float amplitude = 1.0f;
							float total = 0.0f;
							int cells = (int)fmaxf(roundf(meters / 0.09f), 8.0f);

							for (int o = 0; o < 3; o++)
							{
								float n = tiling_noise(u, v, cells << o, o & 1, stone_seed + 61u + (uint32_t)o * 17u);

								ridges += amplitude * (1.0f - 2.0f * fabsf(n));
								total += amplitude;
								amplitude *= 0.55f;
							}
							ridges /= total;
						}
						// (valleys from one, ridges from another turned over)
						// (mostly fine: crumpled like paper, many small planes;
						// a few broad ones under them)
						float crumple = 0.3f * crumpled(u, v, (int)fmaxf(roundf(meters / 0.08f), 8.0f), (int)fmaxf(roundf(meters / 0.04f), 16.0f), 0.4f, meters, stone_seed + 71u)
							- 0.6f * crumpled(u, v, (int)fmaxf(roundf(meters / 0.04f), 8.0f), (int)fmaxf(roundf(meters / 0.018f), 16.0f), 0.5f, meters, stone_seed + 72u)
							+ 1.1f * crumpled(u, v, (int)fmaxf(roundf(meters / 0.024f), 16.0f), (int)fmaxf(roundf(meters / 0.011f), 32.0f), 0.6f, meters, stone_seed + 73u)
							- 0.9f * crumpled(u, v, (int)fmaxf(roundf(meters / 0.011f), 32.0f), (int)fmaxf(roundf(meters / 0.0065f), 64.0f), 0.6f, meters, stone_seed + 75u)
							+ 0.6f * crumpled(u, v, (int)fmaxf(roundf(meters / 0.0035f), 64.0f), (int)fmaxf(roundf(meters / 0.0035f), 64.0f), 0.55f, meters, stone_seed + 74u);
						// Its big faces: 3 to 6 planes, each leaning its own way
						// from round the stone's middle; the stone where all of
						// them are (the lowest), so they meet in sharp ridges
						// (a spalled block's few broad faces)
						float broad = 1e9f;
						float face_light = 0.0f;
						float face_tone = 0.0f;

						{
							int planes = 7 + (int)(hash_unit((int)(stone_seed & 1023u), 40, stone_seed) * 5.0f);
							// (warped: the faces' edges wander, not straight
							// spokes round the middle)
							float lx = (px - middle_x) / fmaxf(half_x, 1e-4f)
								+ 0.25f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.06f), 8.0f), 2, 0.5f, stone_seed + 91u);
							float ly = (py - middle_y) / fmaxf(half_y, 1e-4f)
								+ 0.25f * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.06f), 8.0f), 2, 0.5f, stone_seed + 93u);

							for (int q = 0; q < planes; q++)
							{
								float angle = ((float)q + 0.7f * hash_unit(q, 41, stone_seed)) / (float)planes * 2.0f * PI_F;
								float lean = 0.8f + 1.2f * hash_unit(q, 42, stone_seed);
								float lift = 0.15f * (hash_unit(q, 43, stone_seed) - 0.5f);
								// (in units of the stone's smaller half size)
								float ox = 0.5f * (hash_unit(q, 47, stone_seed) - 0.5f);
								float oy = 0.5f * (hash_unit(q, 48, stone_seed) - 0.5f);
								float h = lift + 0.55f - lean * 0.5f * (cosf(angle) * (lx - ox) + sinf(angle) * (ly - oy));

								if (h < broad)
								{
									broad = h;
									// (the face's own light, as a photograph's: leaning
									// to the upper left, lit; away, in shade)
									face_light = lean * (-0.7f * cosf(angle) - 0.7f * sinf(angle)) * 0.5f;
									face_tone = hash_unit(q, 46, stone_seed) - 0.5f;
								}
							}
							// (and a top plane, its face's flat middle)
							{
								float top_face = 0.45f + 0.1f * hash_unit(9, 44, stone_seed) + 0.05f * lx * (hash_unit(9, 45, stone_seed) - 0.5f);

								if (top_face < broad)
								{
									broad = top_face;
									face_light = 0.1f;
									face_tone = 0.0f;
								}
							}
							broad *= fminf(half_x, half_y) * 0.13f;
						}
						float stone_height = broad + 0.0005f * ridges + 0.32f * crumple + 0.005f * pitched - 0.0035f + tilt * 0.5f + facets * 0.15f + 0.0004f * hammered - 0.002f * pits
							- 0.0012f * scoop;
						// Its color: white to cream, ochre stains, grayer
						// weathered patches, the hammer's fresh light dents,
						// its edges a little shaded down into the joint
						float tone = 0.88f + 0.14f * hash_unit((int)(stone_seed & 1023u), 7, stone_seed);
						float ochre_amount = hash_unit((int)(stone_seed & 1023u), 8, stone_seed);
						float ochre = (ochre_amount > 0.72f ? 0.15f + 0.75f * smooth_step(-0.1f, 0.6f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.08f), 8.0f), 3, 0.55f, stone_seed + 15u)) : 0.0f)
							+ 0.5f * smooth_step(0.0f, -0.0015f, 0.32f * crumple) * smooth_step(0.1f, 0.5f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.05f), 8.0f), 3, 0.55f, stone_seed + 16u));
						float gray = smooth_step(0.2f, 0.6f, tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.06f), 8.0f), 3, 0.6f, stone_seed + 17u));
						float speck = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.0025f), 64.0f), 3, 0.65f, stone_seed + 19u);
						float base[3] = { 1.0f, 0.99f, 0.965f };
						// (the spall's creases: thin dark lines winding over its
						// face where its little planes meet, some of them)
						float crease = 0.0f;

						{
							float warp_u = u + 0.004f / meters * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.02f), 16.0f), 2, 0.5f, stone_seed + 81u);
							float warp_v = v + 0.004f / meters * tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.02f), 16.0f), 2, 0.5f, stone_seed + 83u);
							int cells = (int)fmaxf(roundf(meters / 0.028f), 32.0f);
							int crease_x;
							int crease_y;
							float edge = tiling_cell_edge(warp_u, warp_v, cells, 0.9f, stone_seed + 85u, &crease_x, &crease_y) * meters / (float)cells;
							float some = hash_unit(crease_x, crease_y, stone_seed + 86u) < 0.4f ? 0.6f + 0.4f * hash_unit(crease_x, crease_y, stone_seed + 87u) : 0.0f;

							crease = some * smooth_step(0.0012f, 0.0002f, edge);
						}
						float stain[3] = { 1.0f, 0.86f, 0.68f };

						(void)small;
						// (down to the mortar over its last few millimeters, not a
						// step a pixel wide)
						height = mix_float(mortar_height, fmaxf(stone_height, mortar_height + 0.0005f), smooth_step(0.0f, 0.006f, in));
						for (int k = 0; k < 3; k++)
						{
							float value = mix_float(base[k], stain[k], ochre * 0.6f) * tone;

							value *= 1.0f - 0.03f * gray;
							value *= 1.0f + 0.1f * speck + 0.08f * hammered;
							value *= 1.0f - 0.35f * pits;
							value *= 1.0f - 0.0f * crease;
							value *= 1.0f + 0.03f * fminf(fmaxf(face_light, -1.0f), 1.0f) + 0.03f * face_tone;
							value *= 1.0f + 0.05f * scoop;
							value *= 1.0f - 0.03f * (1.0f - t) * (1.0f - t);
							// (lit from above, as a photograph is: its top a little
							// brighter, its underside in shade)
							value *= 1.0f - 0.07f * clamp_unit((py - middle_y) / fmaxf(half_y, 1e-4f) + 0.2f);
							color[k] = value;
						}
						roughness = 0.82f + 0.1f * pits;
					}
				}
			}
			field->height[index] = height;
			field->roughness[index] = roughness;
			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] = color[k];
			}
		}
	}
	free(wall);
	shade_hollows(field, 2, 0.003f, 0.12f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------
// Layered rock: beds of stone one on another (a mountain's limestone, a
// canyon's sandstone), each cut into blocks by joints across it; the
// blocks' edges worn round, their faces broken and grooved; moss in the
// crevices and on the ledges

#define STRATA_MAX_BEDS 512
#define STRATA_MAX_BLOCKS 96

typedef struct
{
	float bed_minimum;                 // meters
	float bed_maximum;
	float recess;                      // how far a bed may stand back (meters)
	float block_minimum;               // a block's width along its bed (meters)
	float block_maximum;
	float joint_lean;                  // the joints across a bed lean this much (of the bed's thickness)
	float round_minimum;               // the blocks' edges worn round (meters)
	float round_maximum;
	float gap;                         // the joints' width (meters)
	float gap_depth;                   // and depth
	float chunks;                      // how much the faces are broken into planes (0..1)
	float grooves;                     // soft beds worn hollow across (0..1: a canyon's)
	float varnish;                     // dark streaks down the face (0..1: a desert's)
	float cross_bedding;               // fine slanted lines within the beds (0..1)
	float moss;                        // 0..1
	float bed_colors[4][3];            // the beds' colors, one or another (multiplied with the color asked for)
	int bed_color_count;
} strata_style_t;

typedef struct
{
	float bottoms[STRATA_MAX_BEDS + 1];        // v of each bed's start (0..1), the last 1
	int count;
	float block_starts[STRATA_MAX_BEDS][STRATA_MAX_BLOCKS + 1];
	int block_counts[STRATA_MAX_BEDS];
	float recess[STRATA_MAX_BEDS];             // meters
	float hardness[STRATA_MAX_BEDS];           // 0..1
	int color[STRATA_MAX_BEDS];
	float tone[STRATA_MAX_BEDS];
} strata_t;

// The beds down the picture and the blocks along each, at random, filling
// it exactly (so it tiles)
static void strata_lay(strata_t *strata, const strata_style_t *style, float meters, uint32_t seed)
{
	float sum = 0.0f;
	float thickness[STRATA_MAX_BEDS];
	int count = 0;

	while (sum < meters && count < STRATA_MAX_BEDS)
	{
		float t = hash_unit(count, 0, seed);

		thickness[count] = style->bed_minimum + (style->bed_maximum - style->bed_minimum) * t * t;
		sum += thickness[count];
		count++;
	}
	strata->count = count;
	strata->bottoms[0] = 0.0f;
	for (int b = 0; b < count; b++)
	{
		strata->bottoms[b + 1] = strata->bottoms[b] + thickness[b] / sum;
	}
	strata->bottoms[count] = 1.0f;
	for (int b = 0; b < count; b++)
	{
		float widths[STRATA_MAX_BLOCKS];
		float along = 0.0f;
		int blocks = 0;
		float offset = hash_unit(b, 9, seed);

		while (along < meters && blocks < STRATA_MAX_BLOCKS)
		{
			float t = hash_unit(b, 100 + blocks, seed);

			widths[blocks] = style->block_minimum + (style->block_maximum - style->block_minimum) * t;
			along += widths[blocks];
			blocks++;
		}
		strata->block_counts[b] = blocks;
		strata->block_starts[b][0] = offset;
		for (int k = 0; k < blocks; k++)
		{
			strata->block_starts[b][k + 1] = strata->block_starts[b][k] + widths[k] / along;
		}
		strata->hardness[b] = hash_unit(b, 3, seed);
		strata->recess[b] = style->recess * (1.0f - strata->hardness[b]) * (0.4f + 0.6f * hash_unit(b, 4, seed));
		strata->color[b] = style->bed_color_count > 0 ? (int)(hash_unit(b, 5, seed) * (float)style->bed_color_count) % style->bed_color_count : 0;
		strata->tone[b] = 0.9f + 0.2f * hash_unit(b, 6, seed);
	}
}

// Gradient noise stretched: its own number of cells across and down (it
// still tiles): long along one way, short across it (grooves, streaks)
static float stretched_noise(float u, float v, int across, int down, int octaves, uint32_t seed)
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float total = 0.0f;

	for (int o = 0; o < octaves; o++)
	{
		int ax = across << o;
		int dy = down << o;
		float px = u * (float)ax;
		float py = v * (float)dy;
		int ix = (int)floorf(px);
		int iy = (int)floorf(py);
		float fx = px - (float)ix;
		float fy = py - (float)iy;
		float dots[4];

		for (int k = 0; k < 4; k++)
		{
			int cx = wrap_index(ix + (k & 1), ax);
			int cy = wrap_index(iy + (k >> 1), dy);
			float angle = hash_unit(cx, cy, seed + (uint32_t)o * 977u) * 2.0f * PI_F;

			dots[k] = cosf(angle) * (fx - (float)(k & 1)) + sinf(angle) * (fy - (float)(k >> 1));
		}
		{
			float a = fade_curve(fx);
			float b = fade_curve(fy);
			float top = dots[0] + (dots[1] - dots[0]) * a;
			float bottom = dots[2] + (dots[3] - dots[2]) * a;

			sum += amplitude * (top + (bottom - top) * b) * 1.4f;
		}
		total += amplitude;
		amplitude *= 0.5f;
	}
	return sum / total;
}

static int find_interval(const float *starts, int count, float value)
{
	int low = 0;
	int high = count - 1;

	while (low < high)
	{
		int middle = (low + high + 1) / 2;

		if (starts[middle] <= value)
		{
			low = middle;
		}
		else
		{
			high = middle - 1;
		}
	}
	return low;
}

// Moss: soft green cushions where the water and the soil gather (in the
// crevices, along the joints), in patches; strands of it fibrous, a few
// dead, yellow ones
static void grow_moss(field_t *field, const float *crevice, float amount, uint32_t seed)
{
	int size = field->size;

	if (amount <= 0.0f)
	{
		return;
	}
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			float patches = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.5f), 2.0f), 4, 0.55f, seed + 1u);
			float edge = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.03f), 8.0f), 3, 0.6f, seed + 3u);
			float want = crevice[index] * 0.6f + patches * 1.2f + edge * 0.35f - (1.0f - amount) * 1.4f + 0.15f;
			float cover = smooth_step(0.0f, 0.2f, want);
			// Its strands: short fibers every which way, a few millimeters
			float fibers = stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 0.004f), 16.0f),
				(int)fmaxf(roundf(field->meters / 0.0015f), 16.0f), 2, seed + 5u) * 0.5f
				+ stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 0.0015f), 16.0f),
				(int)fmaxf(roundf(field->meters / 0.004f), 16.0f), 2, seed + 7u) * 0.5f;
			float cushion = smooth_step(0.0f, 0.5f, want) * 0.012f;
			float dead = smooth_step(0.55f, 0.75f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.04f), 8.0f), 2, 0.5f, seed + 9u));
			float hue = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.15f), 4.0f), 3, 0.5f, seed + 11u);
			float moss_color[3] =
			{
				0.30f + 0.06f * hue + 0.32f * dead,
				0.34f + 0.05f * hue + 0.22f * dead,
				0.10f + 0.02f * hue - 0.02f * dead,
			};
			float clumps = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.012f), 16.0f), 2, 0.6f, seed + 13u);
			float light = 0.7f + 0.25f * fibers + 0.3f * clumps;

			if (cover <= 0.0f)
			{
				continue;
			}
			field->height[index] += cover * (cushion + fibers * 0.0015f + clumps * 0.003f);
			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] = mix_float(field->color[index * 3 + k], moss_color[k] * light, cover);
			}
			field->roughness[index] = mix_float(field->roughness[index], 0.97f, cover);
		}
	}
}

// Parting k (the bottom of bed k, k any whole number: bed k + count is bed
// k a picture higher) at u: its v, moved along the picture by its own
// noise (a bed thicker here, thinner there, pinching out)
static float strata_parting(const strata_t *strata, int k, float u, uint32_t seed)
{
	int count = strata->count;
	int index = wrap_index(k, count);
	float lap = floorf((float)k / (float)count);
	float thin = fminf(strata->bottoms[index + 1] - strata->bottoms[index],
		strata->bottoms[wrap_index(index - 1, count) + 1] - strata->bottoms[wrap_index(index - 1, count)]);

	return strata->bottoms[index] + lap + 0.45f * thin * tiling_fractal(u, (float)index * 0.618f, 3, 2, 0.5f, seed + 43u);
}

static bool make_strata(const texture_settings_t *settings, const strata_style_t *style, field_t *field)
{
	int size = field->size;
	strata_t *strata = calloc(1, sizeof(strata_t));
	float *crevice = calloc((size_t)size * (size_t)size, sizeof(float));
	int chunk_cells = (int)fmaxf(roundf(field->meters / 0.18f), 4.0f);

	if (strata == NULL || crevice == NULL)
	{
		free(strata);
		free(crevice);
		return false;
	}
	strata_lay(strata, style, field->meters, settings->seed + 7u);
	// (the joints between the blocks: dust and grit, matte)
	fill_roughness(field, 0.9f);
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			// The beds wave a little across the picture
			float bend_v = v + (0.06f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 1.2f), 2.0f), 3, 0.5f, settings->seed + 11u)
				+ 0.012f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.15f), 4.0f), 2, 0.5f, settings->seed + 13u)) / field->meters;
			float wrapped_v = bend_v - floorf(bend_v);
			int bed = find_interval(strata->bottoms, strata->count, wrapped_v);
			// Each parting between beds wanders on its own along the picture
			// (a bed thicker here, thinner there, pinching out): the bed's two
			// partings moved, and the point's bed by them
			float bottom_v;
			float top_v;
			float bed_thickness;
			float across_bed;

			// (in v unwrapped: bed k's bottom is parting k, k counted on past
			// the picture's top and bottom, so the beds go on across them)
			for (int step = 0; step < 4; step++)
			{
				bottom_v = strata_parting(strata, bed, u, settings->seed);
				top_v = strata_parting(strata, bed + 1, u, settings->seed);
				if (wrapped_v < bottom_v)
				{
					bed--;
				}
				else if (wrapped_v > top_v)
				{
					bed++;
				}
				else
				{
					break;
				}
			}
			bed = wrap_index(bed, strata->count);
			bed_thickness = fmaxf(top_v - bottom_v, 1e-4f) * field->meters;
			across_bed = clamp_unit((wrapped_v - bottom_v) / fmaxf(top_v - bottom_v, 1e-6f));
			float to_top = across_bed * bed_thickness;
			float to_bottom = (1.0f - across_bed) * bed_thickness;
			// The joints across it, leaning, wandering
			float lean = style->joint_lean * (hash_unit(bed, 7, settings->seed) - 0.5f) * 2.0f * (across_bed - 0.5f) * bed_thickness / field->meters;
			float bend_u = u + lean + (0.02f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.25f), 4.0f), 2, 0.5f, settings->seed + 15u))
				/ field->meters;
			float along = bend_u - strata->block_starts[bed][0];
			int block;
			float to_left;
			float to_right;
			float edge;
			float round;
			float shoulder;
			float height;

			along -= floorf(along);
			along += strata->block_starts[bed][0];
			block = find_interval(strata->block_starts[bed], strata->block_counts[bed], along);
			to_left = (along - strata->block_starts[bed][block]) * field->meters;
			to_right = (strata->block_starts[bed][block + 1] - along) * field->meters;
			// How far from the block's nearest edge, ragged at a centimeter
			// (a soft minimum of the four: no crease down the block's middle)
			{
				float k = 0.04f;
				float sum = expf(-to_top / k) + expf(-to_bottom / k) + expf(-to_left / k) + expf(-to_right / k);
				float soft = -k * logf(fmaxf(sum, 1e-30f));
				float hard = fminf(fminf(to_top, to_bottom), fminf(to_left, to_right));

				// (ragged near the edge only: no holes in the middle)
				float ragged = (0.008f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.04f), 8.0f), 3, 0.55f, settings->seed + 17u)
					+ 0.018f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.12f), 4.0f), 2, 0.5f, settings->seed + 19u))
					* (1.0f - smooth_step(0.02f, 0.06f, hard)) * fminf(bed_thickness / 0.3f, 1.0f);

				edge = fmaxf(fminf(soft, hard), 0.0f) + ragged;
				// near an edge the hard distance (a sharp gap), the soft one
				// inside (a rounded dome)
				edge = hard < 0.02f ? mix_float(hard, edge, hard / 0.02f) : edge;
			}
			round = style->round_minimum + (style->round_maximum - style->round_minimum) * hash_unit(bed * 97 + block, 11, settings->seed);
			{
				float gap = style->gap * (0.5f + hash_unit(bed * 97 + block, 13, settings->seed));
				float out = clamp_unit((edge - gap * 0.5f) / fmaxf(round, 0.001f));

				// A worn shoulder from the gap's edge up to the face, the face
				// itself bulging a little (pillowy, as weathered blocks are)
				shoulder = sqrtf(fmaxf(1.0f - (1.0f - out) * (1.0f - out), 0.0f));
				// (down into the gap from where the shoulder ends: no step)
				float into = 1.0f - clamp_unit(edge / (gap * 0.5f));

				height = edge < gap * 0.5f ? -round * 1.2f - style->gap_depth * into * into
					: (shoulder - 1.0f) * round * 1.2f + 0.012f * (1.0f - expf(-edge / 0.1f));
				crevice[index] = expf(-fmaxf(edge - gap * 0.5f, 0.0f) / 0.05f);
			}
			// (a bed pinched out to a sliver: one deep crevice, not a stack of
			// little ledges)
			{
				float sliver = 1.0f - smooth_step(style->bed_minimum * 0.5f, style->bed_minimum, bed_thickness);

				height = mix_float(height, -round * 1.2f - style->gap_depth, sliver);
				crevice[index] = fmaxf(crevice[index], sliver);
				shoulder *= 1.0f - sliver;
			}
			// The bed stands back or out; the block a little off its bed
			height -= strata->recess[bed];
			height += (hash_unit(bed * 97 + block, 15, settings->seed) - 0.5f) * 0.015f;
			// A soft bed worn hollow across (a canyon's grooves)
			height -= style->grooves * strata->recess[bed] * sinf(PI_F * across_bed) * 1.5f;
			// The face: broken into planes, grooved along the bed, cracked
			// across it, rough
			{
				int chunk_x;
				int chunk_y;
				float chunks = smooth_facets(u, v, chunk_cells, 0.8f, 14.0f, field->meters, settings->seed + 21u, &chunk_x, &chunk_y)
					+ smooth_facets(u, v, chunk_cells * 3, 0.6f, 16.0f, field->meters, settings->seed + 22u, &chunk_x, &chunk_y) * 0.7f;
				float grooves = stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 0.5f), 2.0f), (int)fmaxf(roundf(field->meters / 0.06f), 8.0f),
					2, settings->seed + 23u) * 0.4f;
				// Fine cracks: the edges of smaller broken pieces, a few of them
				// (two families: along the beds and across them, thin, ending
				// where they will, wandering)
				float warp = 0.01f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.05f), 8.0f), 2, 0.5f, settings->seed + 24u)
					/ field->meters;
				float along_line = 1.0f - fabsf(stretched_noise(u + warp, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f),
					(int)fmaxf(roundf(field->meters / 0.05f), 8.0f), 2, settings->seed + 25u));
				float across_line = 1.0f - fabsf(stretched_noise(u, v + warp, (int)fmaxf(roundf(field->meters / 0.06f), 8.0f),
					(int)fmaxf(roundf(field->meters / 0.25f), 2.0f), 2, settings->seed + 27u));
				float there_along = smooth_step(0.35f, 0.6f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.3f), 4.0f), 2, 0.5f,
					settings->seed + 26u));
				float there_across = smooth_step(0.4f, 0.65f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.3f), 4.0f), 2, 0.5f,
					settings->seed + 29u));
				float line_width = 1.0f - 0.004f / field->meters * 0.0f;
				float cracks = fmaxf(there_along * smooth_step(0.985f, 0.997f, along_line), there_across * smooth_step(0.985f, 0.997f, across_line))
					* line_width;
				float lumps_here = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.15f), 4.0f), 3, 0.5f, settings->seed + 28u);
				float rough = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.05f), 8.0f), 5, 0.6f, settings->seed + 27u);
				float face = shoulder;

				height += face * (chunks * style->chunks + grooves * 0.004f + rough * 0.009f + lumps_here * 0.015f - cracks * 0.008f);
				crevice[index] = fmaxf(crevice[index], cracks * 0.5f);
				// Its color: the bed's, the block's own tone, lighter on the
				// worn edges, darker in the joints and cracks, streaked
				{
					const float *bed_color = style->bed_colors[strata->color[bed]];
					float block_tone = 0.93f + 0.14f * hash_unit(bed * 97 + block, 17, settings->seed);
					float mottle = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.2f), 4.0f), 4, 0.55f, settings->seed + 29u);
					float speckle = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.004f), 16.0f), 2, 0.6f, settings->seed + 31u);
					float edge_light = 0.0f;
					float dark = 0.6f + 0.4f * smooth_step(0.0f, style->gap * 1.5f + 0.006f, edge);
					// (laminae: fine bands along the bed, a few millimeters to
					// centimeters, now and then slanting a little)
					float cross = style->cross_bedding * (0.6f + 0.4f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f), 2, 0.5f,
						settings->seed + 39u)) * (0.5f + 0.5f * sinf(across_bed * bed_thickness * 2.0f * PI_F
						/ (0.008f + 0.012f * hash_unit(bed, 23, settings->seed))
						+ (u * field->meters) * 2.0f * PI_F / 0.3f * (hash_unit(bed, 21, settings->seed) - 0.5f) * 0.2f
						+ 4.0f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.3f), 2.0f), 2, 0.5f, settings->seed + 33u)));
					float varnish = style->varnish * smooth_step(-0.05f, 0.45f, stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 0.06f), 8.0f),
						(int)fmaxf(roundf(field->meters / 1.0f), 2.0f), 3, settings->seed + 35u)) * smooth_step(-0.2f, 0.3f,
						tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 1.5f), 2.0f), 2, 0.5f, settings->seed + 37u));
					float tone = strata->tone[bed] * block_tone * (1.0f + 0.12f * mottle) * (1.0f + 0.07f * speckle) * (1.0f + edge_light)
						* dark * (1.0f - 0.25f * cracks) * (1.0f - 0.07f * cross) * (1.0f + 0.04f * grooves);

					for (int k = 0; k < 3; k++)
					{
						float c = settings->color[k] * bed_color[k] * tone;

						// (desert varnish: dark brown, nearly black)
						field->color[index * 3 + k] = mix_float(c, c * 0.35f * (k == 0 ? 1.1f : (k == 1 ? 0.9f : 0.75f)), varnish);
					}
					// (a worn face: matte, its cracks' grit more; the
					// varnish a thin sheen)
					field->roughness[index] = clamp_unit(0.74f + 0.06f * rough + 0.04f * speckle + 0.15f * cracks - 0.25f * varnish);
				}
			}
			field->height[index] = height;
		}
	}
	shade_hollows(field, 4, 0.02f, 0.35f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	// (the moss over it: its own colors, not the rock's average)
	grow_moss(field, crevice, style->moss, settings->seed + 300u);
	free(strata);
	free(crevice);
	return true;
}

static bool make_rock(const texture_settings_t *settings, field_t *field)
{
	// A mountain's limestone: thick beds and thin, cut into blocks, their
	// edges worn round, gray-blue, mossy
	strata_style_t style =
	{
		.bed_minimum = 0.1f, .bed_maximum = 0.6f, .recess = 0.1f, .block_minimum = 0.25f, .block_maximum = 1.8f, .joint_lean = 0.8f,
		.round_minimum = 0.008f, .round_maximum = 0.03f, .gap = 0.012f, .gap_depth = 0.07f, .chunks = 1.0f, .grooves = 0.0f,
		.varnish = 0.0f, .cross_bedding = 0.0f, .moss = 0.45f,
		.bed_colors = { { 1.0f, 1.0f, 1.0f }, { 0.96f, 0.98f, 1.03f }, { 1.03f, 1.01f, 0.97f }, { 0.92f, 0.94f, 0.97f } }, .bed_color_count = 4,
	};

	if (settings->moss >= 0.0f)
	{
		style.moss = settings->moss;
	}
	return make_strata(settings, &style, field);
}

static bool make_canyon(const texture_settings_t *settings, field_t *field)
{
	// A canyon's sandstone: thin even beds of reds, oranges and creams, the
	// soft ones worn into grooves, few joints, dark varnish streaking down
	strata_style_t style =
	{
		.bed_minimum = 0.06f, .bed_maximum = 0.35f, .recess = 0.05f, .block_minimum = 0.8f, .block_maximum = 3.0f, .joint_lean = 0.6f,
		.round_minimum = 0.01f, .round_maximum = 0.04f, .gap = 0.004f, .gap_depth = 0.02f, .chunks = 0.5f, .grooves = 0.6f,
		.varnish = 0.55f, .cross_bedding = 1.0f, .moss = 0.0f,
		.bed_colors = { { 1.0f, 1.0f, 1.0f }, { 1.08f, 0.92f, 0.82f }, { 0.94f, 0.98f, 1.08f }, { 1.05f, 0.82f, 0.72f } }, .bed_color_count = 4,
	};

	if (settings->moss >= 0.0f)
	{
		style.moss = settings->moss;
	}
	return make_strata(settings, &style, field);
}

// ---------------------------------------------------------------------------
// Dirt: bare earth

// Lumps round points scattered one in each of cells (some of the cells
// none): the highest of the lumps near a point (each its own size, its
// outline pushed about by crumble), 0..1; and the size of that one
static float lumps(float u, float v, int cells, float share, float crumble, uint32_t seed, float *size_out)
{
	float cx = u * (float)cells;
	float cy = v * (float)cells;
	int ix = (int)floorf(cx);
	int iy = (int)floorf(cy);
	float best = 0.0f;

	*size_out = 1.0f;
	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int gx = wrap_index(ix + dx, cells);
			int gy = wrap_index(iy + dy, cells);
			float px;
			float py;
			float size;
			float reach;
			float dome;

			if (hash_unit(gx, gy, seed + 2u) >= share)
			{
				continue;
			}
			px = (float)(ix + dx) + 0.5f + (hash_unit(gx, gy, seed) - 0.5f) * 0.9f;
			py = (float)(iy + dy) + 0.5f + (hash_unit(gx, gy, seed + 1u) - 0.5f) * 0.9f;
			size = 0.6f + 0.8f * hash_unit(gx, gy, seed + 3u);
			reach = clamp_unit(1.0f - (sqrtf((cx - px) * (cx - px) + (cy - py) * (cy - py)) + crumble) / (0.55f * size));
			dome = powf(reach, 0.6f) * smooth_step(0.0f, 0.25f, reach) * size;
			if (dome > best)
			{
				best = dome;
				*size_out = size;
			}
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// Relief: a rock face's big shapes, for the terrain shader (materials.ini's
// relief): gullies down it where the water runs (narrow, long, wandering a
// little), broad buttresses between them, ribs; across u, down v (the side
// projections: v up the face); only its normals and height matter

// ---------------------------------------------------------------------------
// Mud: clay soaked (materials.ini's wet_texture for clay): smooth, its
// cracks closed (faint seams where they were: with the clay's seed, size
// and plates, the same network, so they open in place as it dries), ripple
// marks the water left in places, little pits of drops, a few grains and
// pebbles sunk in it, shallow puddles darker

static bool make_mud(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int big_cells = (int)fmaxf(roundf(field->meters / settings->plate_meters), 2.0f);
	int drop_cells = (int)fmaxf(roundf(field->meters / 0.02f), 8.0f);
	float ripple_angle = 0.4f + 0.8f * hash_unit(3, 9, settings->seed);
	int ripple_across = (int)roundf(field->meters / 0.014f * cosf(ripple_angle));
	int ripple_down = (int)roundf(field->meters / 0.014f * sinf(ripple_angle));

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			// The clay's big cracks, as make_clay finds them (the same seeds)
			float wander = 0.12f / (float)big_cells;
			float rag = 0.0025f / field->meters;
			int rag_period = (int)fmaxf(roundf(field->meters / 0.01f), 8.0f);
			float warp_u = u + wander * tiling_fractal(u, v, big_cells * 2, 2, 0.5f, settings->seed + 11u)
				+ rag * tiling_fractal(u, v, rag_period, 2, 0.5f, settings->seed + 17u);
			float warp_v = v + wander * tiling_fractal(u, v, big_cells * 2, 2, 0.5f, settings->seed + 13u)
				+ rag * tiling_fractal(u, v, rag_period, 2, 0.5f, settings->seed + 19u);
			float seam_edge = tiling_cell_edge(warp_u, warp_v, big_cells, 0.9f, settings->seed + 21u, NULL, NULL)
				* field->meters / (float)big_cells;
			float seam = 1.0f - smooth_step(0.0f, 0.004f + pixel_meters * 2.0f, seam_edge);
			// The surface: a gentle undulation, smoothed by the water
			float undulation = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.3f), 2.0f), 4, 0.5f, settings->seed + 65u);
			float fine = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.01f), 16.0f), 2, 0.5f, settings->seed + 67u);
			// Ripple marks in patches: small, a centimeter and a half apart
			float ripples_there = smooth_step(-0.05f, 0.35f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.8f), 2.0f), 2, 0.5f,
				settings->seed + 71u));
			float ripple_phase = (float)ripple_across * u + (float)ripple_down * v
				+ 1.5f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.08f), 4.0f), 2, 0.5f, settings->seed + 73u);
			float ripple = sinf(ripple_phase * 2.0f * PI_F) * ripples_there;
			// Drops' little pits
			cells_t drop = tiling_cells(u, v, drop_cells, 0.9f, settings->seed + 75u);
			float drop_there = hash_unit(drop.cell_x, drop.cell_y, settings->seed + 77u) < 0.08f ? 1.0f : 0.0f;
			float pit = drop_there * powf(clamp_unit(1.0f - drop.nearest / 0.18f), 2.0f);
			float rim = drop_there * expf(-powf((drop.nearest - 0.2f) / 0.05f, 2.0f));
			// Shallow puddles: the lowest places, flat and darker
			float low = smooth_step(0.25f, 0.6f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.9f), 2.0f), 3, 0.5f,
				settings->seed + 69u)) * 0.6f;
			float height = undulation * 0.006f * (1.0f - 0.7f * low) + fine * 0.0002f + ripple * 0.0005f * (1.0f - low) - pit * 0.0015f
				+ rim * 0.0004f - seam * 0.0006f;
			float mottle = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f), 4, 0.55f, settings->seed + 79u);
			float tone = (1.0f + 0.06f * mottle) * (1.0f + 0.03f * fine) * (1.0f - 0.25f * low) * (1.0f - 0.08f * seam) * (1.0f + 0.03f * ripple) * (1.0f - 0.15f * pit);

			field->height[index] = height;
			field->color[index * 3 + 0] = settings->color[0] * tone;
			field->color[index * 3 + 1] = settings->color[1] * tone;
			field->color[index * 3 + 2] = settings->color[2] * tone * (1.0f + 0.04f * low);
			// (wet and smooth: a sheen, the puddles nearly mirrors, the drops'
			// rims and the seams duller)
			field->roughness[index] = clamp_unit(0.42f + 0.04f * fine - 0.5f * low + 0.1f * rim + 0.1f * seam);
		}
	}
	// A few grains and small pebbles sunk in it
	{
		float grain_pixels = 0.002f / pixel_meters;
		int count = (int)((float)size * (float)size / (grain_pixels * grain_pixels * 900.0f));

		for (int i = 0; i < count; i++)
		{
			uint32_t seed = settings->seed + 6000u;
			float px = hash_unit(i, 0, seed) * (float)size;
			float py = hash_unit(i, 1, seed) * (float)size;
			float radius = (1.0f + 3.0f * powf(hash_unit(i, 2, seed), 4.0f)) * grain_pixels * 0.5f;
			float lightness = 0.7f + 0.5f * hash_unit(i, 3, seed);
			float color[3] = { settings->color[0] * lightness, settings->color[1] * lightness, settings->color[2] * lightness * 1.05f };

			splat_dome(field, px, py, radius, radius * (0.6f + 0.4f * hash_unit(i, 4, seed)), hash_unit(i, 5, seed) * PI_F,
				radius * pixel_meters * 0.5f, 0.6f, color, 0.4f, 0.55f);
		}
	}
	shade_hollows(field, 3, 0.001f, 0.2f);
	bake_light(field, settings->baked_light * 0.6f);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------
// Grass seen from above: blades in tufts, leaning together in patches (as
// the wind and the feet leave them), old dead ones under the living, the
// soil and the thatch showing between; each blade drawn whole, tapering,
// its tip higher (what is higher is drawn over what is lower), darker the
// deeper down

typedef struct
{
	float *depth;       // the height drawn so far at each pixel (meters)
} blade_canvas_t;

// One blade from its base (pixels) out along angle, its length on the
// picture (pixels), its width at the base (pixels), bending (radians over
// it), its tip rising to height (meters) over base_height
static void draw_blade(field_t *field, blade_canvas_t *canvas, float x, float y, float angle, float length, float width, float bend,
	float base_height, float tip_height, const float base_color[3], const float tip_color[3], float roughness)
{
	int size = field->size;
	int steps = (int)fmaxf(length * 2.0f, 2.0f);

	for (int step = 0; step <= steps; step++)
	{
		float t = (float)step / (float)steps;
		float heading = angle + bend * t * t;
		float px = x + cosf(angle) * length * t + (cosf(heading) - cosf(angle)) * length * 0.3f * t;
		float py = y + sinf(angle) * length * t + (sinf(heading) - sinf(angle)) * length * 0.3f * t;
		float half = 0.5f * width * (1.0f - 0.85f * t * t);
		float height = base_height + (tip_height - base_height) * sqrtf(t);
		float across_x = -sinf(heading);
		float across_y = cosf(heading);
		int reach = (int)ceilf(half + 1.0f);

		for (int dy = -reach; dy <= reach; dy++)
		{
			for (int dx = -reach; dx <= reach; dx++)
			{
				float ox = (float)dx + 0.5f - (px - floorf(px));
				float oy = (float)dy + 0.5f - (py - floorf(py));
				float distance = sqrtf(ox * ox + oy * oy);
				float cover = clamp_unit(half + 0.5f - distance);
				int index = wrap_index((int)floorf(py) + dy, size) * size + wrap_index((int)floorf(px) + dx, size);
				// (lit across it: one side of the blade brighter, its middle rib)
				float side = clamp_unit(0.5f + 0.5f * (ox * across_x + oy * across_y) / fmaxf(half, 0.5f));
				float lit = 0.85f + 0.3f * side;

				if (cover <= 0.0f || height <= canvas->depth[index])
				{
					continue;
				}
				canvas->depth[index] = height;
				field->height[index] = mix_float(field->height[index], height, cover);
				for (int k = 0; k < 3; k++)
				{
					float color = mix_float(base_color[k], tip_color[k], t) * lit;

					field->color[index * 3 + k] = mix_float(field->color[index * 3 + k], color, cover);
				}
				field->roughness[index] = mix_float(field->roughness[index], roughness, cover);
			}
		}
	}
}

static bool make_grass(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	blade_canvas_t canvas;
	// Blades per square meter seen from above (most of a lawn's hide under
	// the others): living and dead
	int living = (int)(field->meters * field->meters * 26000.0f);
	int dead = living / 3;
	int tufts = (int)(field->meters * field->meters * 900.0f);
	float soil[3] = { 0.24f, 0.19f, 0.13f };

	canvas.depth = calloc((size_t)size * (size_t)size, sizeof(float));
	if (canvas.depth == NULL)
	{
		return false;
	}
	// The ground: soil and thatch, dark
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			float thatch = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.006f), 16.0f), 2, 0.6f, settings->seed + 11u);
			float tone = 0.8f + 0.3f * thatch;

			field->height[index] = thatch * 0.001f;
			field->color[index * 3 + 0] = soil[0] * tone * 1.1f;
			field->color[index * 3 + 1] = soil[1] * tone;
			field->color[index * 3 + 2] = soil[2] * tone * 0.9f;
			field->roughness[index] = 0.95f;
		}
	}
	// Blades: the dead ones first (lower, lying flatter), then the living;
	// most in tufts round a point, the rest anywhere
	for (int pass = 0; pass < 2; pass++)
	{
		int count = pass == 0 ? dead : living;
		uint32_t seed = settings->seed + (pass == 0 ? 100u : 200u);

		for (int i = 0; i < count; i++)
		{
			float x;
			float y;
			float u;
			float v;
			float lean;
			float angle;
			float stand;
			float blade_meters = (0.03f + 0.07f * hash_unit(i, 3, seed)) * (pass == 0 ? 1.2f : 1.0f);
			float length;
			float width = fmaxf((0.0018f + 0.0022f * hash_unit(i, 4, seed)) / pixel_meters, 0.8f);
			float tip_height;
			float base_color[3];
			float tip_color[3];

			if (hash_unit(i, 0, seed) < 0.7f)
			{
				int tuft = (int)(hash_unit(i, 1, seed) * (float)tufts);
				float spread = (0.008f + 0.02f * hash_unit(tuft, 9, seed)) / pixel_meters;
				float r = spread * sqrtf(-2.0f * logf(fmaxf(hash_unit(i, 2, seed), 1e-6f)));
				float a = hash_unit(i, 5, seed) * 2.0f * PI_F;

				x = hash_unit(tuft, 7, seed) * (float)size + cosf(a) * r;
				y = hash_unit(tuft, 8, seed) * (float)size + sinf(a) * r;
			}
			else
			{
				x = hash_unit(i, 1, seed) * (float)size;
				y = hash_unit(i, 2, seed) * (float)size;
			}
			u = x / (float)size - floorf(x / (float)size);
			v = y / (float)size - floorf(y / (float)size);
			// They lean together in patches, each its own way a little
			lean = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f), 2, 0.5f, settings->seed + 21u) * PI_F * 1.5f;
			angle = lean + (hash_unit(i, 6, seed) - 0.5f) * 2.2f;
			// How upright: seen from above an upright blade is short, a
			// lying one long; the dead lie flatter
			stand = (pass == 0 ? 0.15f : 0.35f) + (pass == 0 ? 0.35f : 0.55f) * hash_unit(i, 10, seed);
			length = blade_meters * cosf(stand) / pixel_meters;
			tip_height = blade_meters * sinf(stand) + (pass == 0 ? 0.0f : 0.01f);
			if (pass == 0)
			{
				float straw = 0.75f + 0.35f * hash_unit(i, 11, seed);

				base_color[0] = 0.42f * straw;
				base_color[1] = 0.36f * straw;
				base_color[2] = 0.24f * straw;
				tip_color[0] = 0.66f * straw;
				tip_color[1] = 0.58f * straw;
				tip_color[2] = 0.40f * straw;
			}
			else
			{
				// Each its own green: yellower, bluer, lighter; patches of a
				// yellower green
				float yellow = clamp_unit(hash_unit(i, 12, seed) * 0.6f
					+ 0.5f * tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.5f), 2.0f), 2, 0.5f, settings->seed + 31u));
				float light = 0.85f + 0.3f * hash_unit(i, 13, seed);

				for (int k = 0; k < 3; k++)
				{
					float green = settings->color[k] * light;
					float yellowed = settings->color[k] * light * (k == 0 ? 1.45f : (k == 1 ? 1.12f : 0.8f));

					tip_color[k] = mix_float(green, yellowed, yellow * 0.6f) * 1.15f;
					base_color[k] = mix_float(green, yellowed, yellow * 0.3f) * 0.75f;
				}
			}
			draw_blade(field, &canvas, x, y, angle, length, width, (hash_unit(i, 14, seed) - 0.5f) * 1.2f,
				pass == 0 ? 0.0f : 0.004f * hash_unit(i, 15, seed), tip_height, base_color, tip_color,
				pass == 0 ? 0.85f : 0.68f + 0.12f * hash_unit(i, 16, seed));
		}
	}
	// Deeper down, darker: the light does not reach there
	{
		float deepest = 0.05f;

#pragma omp parallel for schedule(static)
		for (int i = 0; i < size * size; i++)
		{
			float shade = 0.35f + 0.65f * clamp_unit(canvas.depth[i] / deepest);

			for (int k = 0; k < 3; k++)
			{
				field->color[i * 3 + k] *= sqrtf(shade);
			}
		}
	}
	free(canvas.depth);
	bake_light(field, settings->baked_light * 0.5f);
	// (the normals gentler than the blades really stand: a blade a pixel or
	// two wide standing up would make them noise)
	for (int i = 0; i < size * size; i++)
	{
		field->height[i] *= 0.12f;
	}
	match_average(field, settings->color);
	return true;
}

static bool make_relief(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	int gully_period = (int)fmaxf(roundf(field->meters / 14.0f), 2.0f);
	int gully_down = (int)fmaxf(roundf(field->meters / 110.0f), 1.0f);

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			float wander = stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 40.0f), 1.0f), (int)fmaxf(roundf(field->meters / 40.0f), 1.0f), 2,
				settings->seed + 1u) * 5.0f / field->meters;
			float gully = 1.0f - fabsf(stretched_noise(u + wander, v, gully_period, gully_down, 3, settings->seed + 3u));
			float buttress = stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 30.0f), 1.0f), (int)fmaxf(roundf(field->meters / 90.0f), 1.0f), 2,
				settings->seed + 5u);
			float rib = stretched_noise(u, v, (int)fmaxf(roundf(field->meters / 2.5f), 4.0f), (int)fmaxf(roundf(field->meters / 14.0f), 1.0f), 3,
				settings->seed + 7u);

			field->height[index] = -powf(gully, 8.0f) * 3.5f + buttress * 1.6f + rib * 0.3f;
			field->color[index * 3 + 0] = 0.5f;
			field->color[index * 3 + 1] = 0.5f;
			field->color[index * 3 + 2] = 0.5f;
		}
	}
	return true;
}

static bool make_dirt(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int clod_cells = (int)fmaxf(roundf(field->meters / 0.03f), 8.0f);
	int crumb_cells = (int)fmaxf(roundf(field->meters / 0.006f), 16.0f);

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			float warp_u = u + 0.4f / (float)clod_cells * tiling_fractal(u, v, clod_cells, 2, 0.5f, settings->seed + 11u);
			float warp_v = v + 0.4f / (float)clod_cells * tiling_fractal(u, v, clod_cells, 2, 0.5f, settings->seed + 13u);
			// Clods: lumps of a few centimeters, rounded, each a little
			// different; crumbs on them and between
			// (their outline crumbly: the distance pushed about at a few
			// millimeters)
			float crumble = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.006f), 16.0f), 2, 0.6f, settings->seed + 27u) * 0.12f;
			float clod_size;
			float clod_dome = lumps(warp_u, warp_v, clod_cells, 0.55f, crumble, settings->seed + 21u, &clod_size) / fmaxf(clod_size, 0.01f);
			cells_t crumb = tiling_cells(u, v, crumb_cells, 0.95f, settings->seed + 31u);
			float crumb_dome = sqrtf(fmaxf(1.0f - powf(crumb.nearest / 0.55f, 2.0f), 0.0f))
				* (hash_unit(crumb.cell_x, crumb.cell_y, settings->seed + 33u) < 0.6f ? 1.0f : 0.3f);
			float undulation = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.3f), 2.0f), 4, 0.55f, settings->seed + 41u);
			float fine = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.004f), 8.0f), 2, 0.6f, settings->seed + 43u);
			float height = undulation * 0.008f + clod_dome * 0.008f * clod_size + crumb_dome * 0.0015f + fine * 0.0003f;
			// Its color: the earth's, damper and darker in patches, the
			// clods' tops drier and paler, a crumb lighter or darker,
			// a little organic matter darker still
			float damp = smooth_step(-0.1f, 0.6f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.25f), 2.0f), 4, 0.55f,
				settings->seed + 51u)) * 0.55f;
			float crumb_tone = 0.85f + 0.3f * hash_unit(crumb.cell_x, crumb.cell_y, settings->seed + 53u);
			float dry_top = 1.0f + 0.12f * clod_dome;
			float organic = smooth_step(0.6f, 0.9f, tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.08f), 4.0f), 2, 0.5f,
				settings->seed + 55u));
			float tone = crumb_tone * dry_top * (1.0f - 0.22f * damp) * (1.0f - 0.3f * organic) * (1.0f + 0.04f * fine);

			field->height[index] = height;
			field->color[index * 3 + 0] = settings->color[0] * tone;
			field->color[index * 3 + 1] = settings->color[1] * tone * (1.0f - 0.03f * damp);
			field->color[index * 3 + 2] = settings->color[2] * tone * (1.0f - 0.06f * damp);
			field->roughness[index] = clamp_unit(0.95f - 0.15f * damp + 0.03f * fine);
		}
	}
	// Small stones in it, and bits of dead plants: thin dark slivers
	{
		int stones = (int)(field->meters * field->meters * 260.0f);
		int slivers = (int)(field->meters * field->meters * 160.0f);

		for (int i = 0; i < stones; i++)
		{
			uint32_t seed = settings->seed + 4000u;
			float x = hash_unit(i, 0, seed) * (float)size;
			float y = hash_unit(i, 1, seed) * (float)size;
			float radius = (0.002f + 0.012f * powf(hash_unit(i, 2, seed), 3.0f)) / pixel_meters;
			// (gray to brown stones, dirty: some of the earth's own color on
			// them)
			float gray = 0.32f + 0.3f * hash_unit(i, 3, seed);
			float warm = hash_unit(i, 4, seed) * 0.12f;
			float dirty = 0.3f + 0.4f * hash_unit(i, 7, seed);
			float color[3] =
			{
				mix_float(gray * (1.0f + warm), settings->color[0], dirty),
				mix_float(gray * 0.97f, settings->color[1], dirty),
				mix_float(gray * (0.92f - warm), settings->color[2], dirty),
			};

			splat_dome(field, x, y, radius, radius * (0.55f + 0.45f * hash_unit(i, 5, seed)), hash_unit(i, 6, seed) * PI_F,
				radius * pixel_meters * 0.6f, 0.55f, color, 0.6f, 0.65f + 0.2f * hash_unit(i, 8, seed));
		}
		for (int i = 0; i < slivers; i++)
		{
			uint32_t seed = settings->seed + 5000u;
			float x = hash_unit(i, 0, seed) * (float)size;
			float y = hash_unit(i, 1, seed) * (float)size;
			float length = (0.006f + 0.03f * hash_unit(i, 2, seed)) / pixel_meters;
			// (old straw, pale, or rotting, dark)
			float straw = hash_unit(i, 5, seed) < 0.4f ? 1.0f : 0.0f;
			float shade = straw > 0.5f ? 1.15f + 0.3f * hash_unit(i, 3, seed) : 0.4f + 0.3f * hash_unit(i, 3, seed);
			float color[3] = { settings->color[0] * shade * 1.08f, settings->color[1] * shade, settings->color[2] * shade * (straw > 0.5f ? 0.75f : 0.85f) };

			splat_dome(field, x, y, length * 0.5f, fmaxf(0.0008f / pixel_meters, 0.7f), hash_unit(i, 4, seed) * PI_F,
				0.0008f, 0.3f, color, 0.4f, straw > 0.5f ? 0.7f : 0.9f);
		}
	}
	shade_hollows(field, 3, 0.003f, 0.45f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------
// What lies on the ground under things (the material map's covers): fallen
// needles, fallen leaves, trampled earth; and the petals a blooming tree
// drops (an overlay: its _mask.png says in what order they show)

// One leaf: a lens (pointed at both ends) at x, y (pixels), length and
// width (pixels), turned; curled up at its edges (curl, meters), its
// midrib darker
static void splat_leaf(field_t *field, float x, float y, float length, float width, float angle, float curl, const float color[3],
	float roughness)
{
	int size = field->size;
	float reach = length * 0.5f + 1.5f;
	float c = cosf(angle);
	float s = sinf(angle);
	float base = field->height[wrap_index((int)y, size) * size + wrap_index((int)x, size)];

	for (int py = (int)floorf(y - reach); py <= (int)ceilf(y + reach); py++)
	{
		for (int px = (int)floorf(x - reach); px <= (int)ceilf(x + reach); px++)
		{
			float dx = (float)px + 0.5f - x;
			float dy = (float)py + 0.5f - y;
			float along = (dx * c + dy * s) / (length * 0.5f);
			float across = -dx * s + dy * c;
			float half = width * 0.5f * powf(fmaxf(1.0f - along * along, 0.0f), 0.7f);
			float cover = clamp_unit(half + 0.5f - fabsf(across));
			int index = wrap_index(py, size) * size + wrap_index(px, size);
			float edge = half > 0.5f ? fabsf(across) / half : 1.0f;
			float rib = clamp_unit(1.0f - fabsf(across) / fmaxf(width * 0.04f, 0.6f)) * (1.0f - fabsf(along)) * 0.35f;
			float top;

			if (cover <= 0.0f || fabsf(along) > 1.0f)
			{
				continue;
			}
			top = base + 0.0004f + curl * edge * edge;
			field->height[index] = mix_float(field->height[index], top, cover);
			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] = mix_float(field->color[index * 3 + k], color[k] * (1.0f - rib) * (0.9f + 0.15f * edge), cover);
			}
			field->roughness[index] = mix_float(field->roughness[index], roughness, cover);
		}
	}
}

// Earth under the litter: the dirt's, darker and finer (humus)
static void make_humus(const texture_settings_t *settings, field_t *field, float tone)
{
	texture_settings_t under = *settings;

	for (int k = 0; k < 3; k++)
	{
		under.color[k] = settings->color[k] * tone;
	}
	make_dirt(&under, field);
}

static bool make_needles(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	// (fewer than the real litter has, longer and wider than real needles:
	// at a millimeter or two a pixel real ones blur into dirt; these read
	// as needles, the dark humus showing between them)
	int needles = (int)(field->meters * field->meters * 4500.0f);
	int cones = (int)(field->meters * field->meters * 2.0f);
	int twigs = (int)(field->meters * field->meters * 6.0f);

	make_humus(settings, field, 0.38f);
	// (no pale pebbles in a forest's litter: the humus' light grains as dark
	// as the rest of it)
	for (int i = 0; i < size * size; i++)
	{
		for (int k = 0; k < 3; k++)
		{
			field->color[i * 3 + k] = fminf(field->color[i * 3 + k], settings->color[k] * 0.5f);
		}
	}
	// Bare twigs fallen first: thin, gray brown, a little crooked
	for (int i = 0; i < twigs; i++)
	{
		uint32_t seed = settings->seed + 7700u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float length = (0.08f + 0.18f * hash_unit(i, 2, seed)) / pixel_meters;
		float angle = hash_unit(i, 3, seed) * PI_F;
		float color[3] = { 0.30f, 0.25f, 0.20f };
		int pieces = 3;

		for (int k = 0; k < pieces; k++)
		{
			float bend = angle + (hash_unit(i, 10 + k, seed) - 0.5f) * 0.5f;
			float step = length / (float)pieces;
			float along = ((float)k + 0.5f) * step - 0.5f * length;

			splat_dome(field, x + cosf(angle) * along, y + sinf(angle) * along, step * 0.55f, fmaxf(0.002f / pixel_meters, 1.0f), bend, 0.002f,
				0.1f, color, 0.4f, 0.85f);
		}
	}
	// Layer on layer: the old ones (gray brown) under the newer (rust, a
	// few still green). In drifts: where they lie thick, mostly one way
	// (the wind, the rain that moved them), and thinner between
	for (int i = 0; i < needles; i++)
	{
		uint32_t seed = settings->seed + 7000u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float u = x / (float)size;
		float v = y / (float)size;
		float drift = tiling_fractal(u, v, 3, 3, 0.5f, seed + 11u);
		float length = (0.025f + 0.02f * hash_unit(i, 2, seed)) / pixel_meters;
		float width = fmaxf(0.0016f / pixel_meters, 1.1f);
		float age = (float)i / (float)needles;
		float pick = hash_unit(i, 3, seed);
		float shade = 0.75f + 0.45f * hash_unit(i, 4, seed);
		float flow = tiling_fractal(u, v, 2, 2, 0.5f, seed + 23u) * PI_F;
		float angle = hash_unit(i, 7, seed) < 0.6f ? flow + (hash_unit(i, 5, seed) - 0.5f) * 0.9f : hash_unit(i, 5, seed) * PI_F;
		float color[3];

		// (thin where the drifts are: some of them left out)
		if (hash_unit(i, 8, seed) > 0.55f + 0.6f * drift)
		{
			continue;
		}
		if (pick < 0.07f && age > 0.6f)
		{
			color[0] = 0.26f * shade; color[1] = 0.33f * shade; color[2] = 0.12f * shade;
		}
		else
		{
			float old = 1.0f - age;

			color[0] = mix_float(0.62f, 0.34f, old * 0.8f) * shade;
			color[1] = mix_float(0.33f, 0.28f, old * 0.8f) * shade;
			color[2] = mix_float(0.13f, 0.21f, old * 0.8f) * shade;
		}
		splat_dome(field, x, y, length * 0.5f, width * 0.5f + 0.3f, angle, 0.0008f + 0.0025f * age, 0.2f, color, 0.3f,
			0.65f + 0.15f * hash_unit(i, 6, seed));
	}
	// A cone here and there: a long egg, its scales in rows across it
	for (int i = 0; i < cones; i++)
	{
		uint32_t seed = settings->seed + 7500u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float length = (0.08f + 0.06f * hash_unit(i, 2, seed)) / pixel_meters;
		float angle = hash_unit(i, 3, seed) * PI_F;
		int rows = 14;

		// (its scales overlapping, each row a little lighter at its tip)
		for (int k = 0; k < rows; k++)
		{
			float t = ((float)k + 0.5f) / (float)rows - 0.5f;
			float radius = length * 0.18f * sqrtf(fmaxf(1.0f - 4.0f * t * t, 0.2f));
			float light = 0.85f + 0.12f * (float)(k & 1) + 0.1f * hash_unit(i * 31 + k, 4, seed);
			float color[3] = { 0.30f * light, 0.19f * light, 0.11f * light };

			splat_dome(field, x + cosf(angle) * t * length, y + sinf(angle) * t * length, length * 0.07f, radius, angle,
				radius * pixel_meters * 0.9f, 0.1f, color, 0.5f, 0.8f);
		}
	}
	shade_hollows(field, 3, 0.002f, 0.6f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

static bool make_leaves(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int leaves = (int)(field->meters * field->meters * 700.0f);

	make_humus(settings, field, 0.5f);
	// The rotten ones under, dark; the newly fallen on top, the autumn's
	// colors: brown, ochre, orange, a few still yellow or red
	for (int i = 0; i < leaves; i++)
	{
		uint32_t seed = settings->seed + 8000u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float length = (0.04f + 0.06f * hash_unit(i, 2, seed)) / pixel_meters;
		float width = length * (0.4f + 0.25f * hash_unit(i, 3, seed));
		float age = (float)i / (float)leaves;
		float pick = hash_unit(i, 4, seed);
		float shade = 0.8f + 0.35f * hash_unit(i, 5, seed);
		float color[3];

		if (age < 0.4f)
		{
			color[0] = 0.22f * shade; color[1] = 0.16f * shade; color[2] = 0.10f * shade;
		}
		else if (pick < 0.12f)
		{
			color[0] = 0.72f * shade; color[1] = 0.56f * shade; color[2] = 0.16f * shade;
		}
		else if (pick < 0.2f)
		{
			color[0] = 0.62f * shade; color[1] = 0.22f * shade; color[2] = 0.10f * shade;
		}
		else
		{
			color[0] = settings->color[0] * 1.3f * shade;
			color[1] = settings->color[1] * 1.2f * shade;
			color[2] = settings->color[2] * 1.0f * shade;
		}
		splat_leaf(field, x, y, length, width, hash_unit(i, 6, seed) * 2.0f * PI_F, 0.002f + 0.004f * hash_unit(i, 7, seed), color,
			0.6f + 0.25f * hash_unit(i, 8, seed));
	}
	shade_hollows(field, 4, 0.003f, 0.55f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

static bool make_trampled(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int straws = (int)(field->meters * field->meters * 120.0f);
	float lowest = 1e9f;

	make_dirt(settings, field);
	// Pressed flat: the clods' tops cut off, smoother, a little shinier
	for (int i = 0; i < size * size; i++)
	{
		lowest = fminf(lowest, field->height[i]);
	}
	for (int i = 0; i < size * size; i++)
	{
		float above = field->height[i] - lowest;

		field->height[i] = lowest + fminf(above, 0.004f) * 0.5f + fmaxf(above - 0.004f, 0.0f) * 0.1f;
		field->roughness[i] = fminf(field->roughness[i], 0.8f);
	}
	// Dead grass pressed into it, lying the ways people walk
	for (int i = 0; i < straws; i++)
	{
		uint32_t seed = settings->seed + 9000u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float length = (0.02f + 0.05f * hash_unit(i, 2, seed)) / pixel_meters;
		float shade = 0.9f + 0.3f * hash_unit(i, 3, seed);
		float color[3] = { 0.58f * shade, 0.50f * shade, 0.34f * shade };
		float angle = (hash_unit(i, 4, seed) < 0.5f ? 0.3f : 1.8f) + (hash_unit(i, 5, seed) - 0.5f) * 0.6f;

		splat_dome(field, x, y, length * 0.5f, fmaxf(0.001f / pixel_meters, 0.7f), angle, 0.0005f, 0.3f, color, 0.3f, 0.75f);
	}
	shade_hollows(field, 3, 0.002f, 0.35f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

// Petals: the color under them is the petals' own (it never shows: the mask
// keeps it out); the mask gives each petal its turn (0 first .. 1 never)
static bool make_petals(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	int petals = (int)(field->meters * field->meters * 2500.0f);

	field->mask = malloc((size_t)size * (size_t)size * sizeof(float));
	if (field->mask == NULL)
	{
		return false;
	}
	for (int i = 0; i < size * size; i++)
	{
		field->height[i] = 0.0f;
		field->mask[i] = 1.0f;
		field->roughness[i] = 0.6f;
		for (int k = 0; k < 3; k++)
		{
			field->color[i * 3 + k] = settings->color[k];
		}
	}
	for (int i = 0; i < petals; i++)
	{
		uint32_t seed = settings->seed + 9500u;
		float x = hash_unit(i, 0, seed) * (float)size;
		float y = hash_unit(i, 1, seed) * (float)size;
		float length = (0.008f + 0.007f * hash_unit(i, 2, seed)) / pixel_meters;
		float shade = 0.85f + 0.2f * hash_unit(i, 3, seed);
		float white = hash_unit(i, 4, seed);
		float color[3] =
		{
			mix_float(settings->color[0], 0.97f, white * 0.6f) * shade,
			mix_float(settings->color[1], 0.95f, white * 0.6f) * shade,
			mix_float(settings->color[2], 0.96f, white * 0.6f) * shade,
		};
		float turn = 0.02f + 0.96f * hash_unit(i, 5, seed);
		float angle = hash_unit(i, 6, seed) * 2.0f * PI_F;
		float reach = length * 0.5f + 1.5f;

		splat_leaf(field, x, y, length, length * 0.75f, angle, 0.0006f, color, 0.55f);
		// (its turn where it lies: the lowest wins where they overlap)
		for (int py = (int)floorf(y - reach); py <= (int)ceilf(y + reach); py++)
		{
			for (int px = (int)floorf(x - reach); px <= (int)ceilf(x + reach); px++)
			{
				float dx = (float)px + 0.5f - x;
				float dy = (float)py + 0.5f - y;
				float along = (dx * cosf(angle) + dy * sinf(angle)) / (length * 0.5f);
				float across = -dx * sinf(angle) + dy * cosf(angle);
				float half = length * 0.75f * 0.5f * powf(fmaxf(1.0f - along * along, 0.0f), 0.7f);
				int index = wrap_index(py, size) * size + wrap_index(px, size);

				if (fabsf(along) <= 1.0f && fabsf(across) <= half)
				{
					field->mask[index] = fminf(field->mask[index], turn);
				}
			}
		}
	}
	bake_light(field, settings->baked_light * 0.5f);
	return true;
}

// Snow: soft drifts, wind crust here and there, the light blue in its
// hollows; its mask (an overlay's: snow settles over the ground in its
// order) in patches a few decimeters across, fine at their edges
static bool make_snow(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;

	field->mask = malloc((size_t)size * (size_t)size * sizeof(float));
	if (field->mask == NULL)
	{
		return false;
	}
#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int index = y * size + x;
			float drifts = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.6f), 2.0f), 4, 0.5f, settings->seed + 11u);
			float crust = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.05f), 4.0f), 3, 0.55f, settings->seed + 13u);
			float grain = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.004f), 16.0f), 2, 0.6f, settings->seed + 17u);
			float patches = tiling_fractal(u, v, (int)fmaxf(roundf(field->meters / 0.4f), 2.0f), 5, 0.6f, settings->seed + 19u);
			float height = drifts * 0.02f + crust * 0.002f + grain * 0.0003f;
			float hollow = clamp_unit(0.5f - drifts * 0.8f);
			float tone = 0.97f + 0.03f * grain;

			field->height[index] = height;
			field->color[index * 3 + 0] = settings->color[0] * tone * (1.0f - 0.06f * hollow);
			field->color[index * 3 + 1] = settings->color[1] * tone * (1.0f - 0.03f * hollow);
			field->color[index * 3 + 2] = settings->color[2] * tone;
			field->roughness[index] = clamp_unit(0.62f - 0.12f * smooth_step(0.2f, 0.6f, crust) + 0.05f * grain);
			field->mask[index] = clamp_unit(0.5f + 0.8f * patches + 0.1f * crust);
		}
	}
	bake_light(field, settings->baked_light * 0.6f);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------
// Soaked: any of them after the rain (wet_sand, wet_dirt). The water fills
// the pores and the ground settles: the grains' and crumbs' relief softer,
// the colors deeper and darker, a sheen all over; in the lowest places
// (between the ripples, between the clods) water standing, a smooth film

// (settle: how much of the finest relief goes, 0..1)
static bool soak(const texture_settings_t *settings, field_t *field, float settle)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	float *settled = malloc((size_t)size * (size_t)size * sizeof(float));
	float *wide = malloc((size_t)size * (size_t)size * sizeof(float));
	int histogram[4096];
	float lowest = 1e9f;
	float highest = -1e9f;
	float level;
	float meniscus;

	if (settled == NULL || wide == NULL)
	{
		free(settled);
		free(wide);
		return false;
	}
	// Settled: the finest relief (a grain, a crumb) mostly gone, a
	// clod's crumbs slumped together; and its color as smudged a little
	memcpy(settled, field->height, (size_t)size * (size_t)size * sizeof(float));
	blur_wrapping(settled, size, (int)fminf(fmaxf(settle * 0.003f / pixel_meters, 1.0f), 6.0f));
	for (int i = 0; i < size * size; i++)
	{
		field->height[i] = mix_float(field->height[i], settled[i], settle);
	}
	for (int k = 0; k < 3; k++)
	{
		for (int i = 0; i < size * size; i++)
		{
			wide[i] = field->color[i * 3 + k];
		}
		blur_wrapping(wide, size, (int)fminf(fmaxf(settle * 0.002f / pixel_meters, 1.0f), 4.0f));
		for (int i = 0; i < size * size; i++)
		{
			field->color[i * 3 + k] = mix_float(field->color[i * 3 + k], wide[i], settle * 0.6f);
		}
	}
	// Where water stands: the lowest share of the ground measured over a
	// centimeter or so (not in every grain's gap), its level from the
	// heights' spread
	memcpy(wide, field->height, (size_t)size * (size_t)size * sizeof(float));
	blur_wrapping(wide, size, (int)fminf(fmaxf(0.004f / pixel_meters, 1.0f), 8.0f));
	for (int i = 0; i < size * size; i++)
	{
		lowest = fminf(lowest, wide[i]);
		highest = fmaxf(highest, wide[i]);
	}
	memset(histogram, 0, sizeof(histogram));
	for (int i = 0; i < size * size; i++)
	{
		int bin = (int)((wide[i] - lowest) / fmaxf(highest - lowest, 1e-9f) * 4095.0f);

		histogram[bin]++;
	}
	{
		long want = (long)((double)settings->puddles * (double)size * (double)size);
		long seen = 0;
		int bin = 0;

		while (bin < 4095 && seen + histogram[bin] < want)
		{
			seen += histogram[bin];
			bin++;
		}
		level = lowest + ((float)bin + 0.5f) / 4095.0f * (highest - lowest);
	}
	// (the water's edge a little soft: it creeps up the grains)
	meniscus = fmaxf((highest - lowest) * 0.02f, 0.00005f);
#pragma omp parallel for schedule(static)
	for (int i = 0; i < size * size; i++)
	{
		float water = settings->puddles > 0.0f ? smooth_step(level + meniscus, level - meniscus, wide[i]) : 0.0f;
		float luminance = field->color[i * 3] * 0.3f + field->color[i * 3 + 1] * 0.59f + field->color[i * 3 + 2] * 0.11f;

		for (int k = 0; k < 3; k++)
		{
			// (deeper: the color pulled away from its gray, then darker; a
			// little darker still under the water)
			float c = luminance + (field->color[i * 3 + k] - luminance) * 1.25f;

			field->color[i * 3 + k] = clamp_unit(c * 0.8f * (1.0f - 0.2f * water));
		}
		// The water's top flat: its level over what is under it
		field->height[i] = mix_float(field->height[i], fmaxf(field->height[i], level + (field->height[i] - wide[i]) * 0.1f), water);
		field->roughness[i] = mix_float(field->roughness[i] * 0.4f, 0.05f, water);
	}
	free(settled);
	free(wide);
	return true;
}

// ---------------------------------------------------------------------------
// Weatherboards: old boards laid across the picture, each one's lower edge
// over the top of the one under it (a waney edge: wandering, chipped), its
// face leaning out to that edge. Each board is its own piece of a log: its
// rings the log's cut along it (arches where the cut nears the pith), the
// soft wood between them weathered away into furrows, the grain flowing
// round its knots, checks opening along the grain. Hand made nails in
// rows up the studs, rust run down from them; lichen rosettes and crust
// along the lower edges where the water stays; dark mold in streaks.
// x along the boards and y down the picture, both in meters.

#define WEATHERBOARD_KNOTS 4
#define WEATHERBOARD_CHECKS 10
#define WEATHERBOARD_NAILS 8
#define WEATHERBOARD_LICHENS 96

typedef struct
{
	float x;                           // along the board (meters)
	float y;                           // across it, from its middle line (meters)
	float radius;                      // meters
	float turn;                        // its rings' and checks' turn (radians)
	float dead;                        // 0 a sound knot .. 1 a dead one (dark, a gap round it)
} weatherboard_knot_t;

typedef struct
{
	float x;                           // its middle along the board (meters)
	float level;                       // which line of the grain it follows (meters from the board's middle)
	float half_length;                 // meters
	float width;                       // at its middle (meters)
} weatherboard_check_t;

typedef struct
{
	float x;                           // meters
	float y;                           // down from the board's own top line (meters)
	float radius;                      // its head's (meters)
	float streak;                      // how far the rust ran down (meters)
} weatherboard_nail_t;

typedef struct
{
	float x;                           // meters
	float y;                           // down from the board's own top line (meters)
	float radius;                      // meters
	float lobes;                       // how many lobes round it
	float turn;                        // radians
	float yellow;                      // 0 gray green .. 1 yellow green
} weatherboard_lichen_t;

typedef struct
{
	uint32_t seed;
	float tone;                        // lighter or darker than the others
	float gray;                        // how weathered to gray (0 .. 1)
	float warm;                        // redder or yellower than the others (-1 .. 1)
	float ring_spacing;                // meters between its furrows
	float pith_across;                 // where the log's pith is across the board (meters from its middle)
	float pith_depth;                  // how far under the face (meters)
	float pith_wander;                 // by how much that changes along the board (meters)
	float waviness;                    // its lower edge's (about 1)
	int knot_count;
	weatherboard_knot_t knots[WEATHERBOARD_KNOTS];
	int check_count;
	weatherboard_check_t checks[WEATHERBOARD_CHECKS];
	int nail_count;
	weatherboard_nail_t nails[WEATHERBOARD_NAILS];
	int lichen_count;
	weatherboard_lichen_t lichens[WEATHERBOARD_LICHENS];
} weatherboard_board_t;

typedef struct
{
	int count;                         // boards down the picture
	float board_height;                // what shows of each (meters)
	float meters;
	float lichen;                      // how much lichen (0 .. 1)
	uint32_t seed;
	weatherboard_board_t *boards;
} weatherboard_wall_t;

// Where board number's lower edge is at x (meters down the picture; number
// any whole number: board number + count is board number a picture lower)
static float weatherboard_edge(const weatherboard_wall_t *wall, int number, float x)
{
	const weatherboard_board_t *board = &wall->boards[wrap_index(number, wall->count)];
	float u = x / wall->meters;
	int long_period = (int)fmaxf(roundf(wall->meters / 0.4f), 2.0f);
	int short_period = (int)fmaxf(roundf(wall->meters / 0.09f), 4.0f);
	int chip_period = (int)fmaxf(roundf(wall->meters / 0.014f), 8.0f);
	float scale = fminf(wall->board_height / 0.2f, 1.0f) * board->waviness;
	float chips = cell_noise(u * (float)chip_period, 0.43f, chip_period, board->seed + 3u);

	// (chips: bites out of the edge, never bits added to it)
	return (float)(number + 1) * wall->board_height + scale * (0.016f * cell_noise(u * (float)long_period, 0.37f, long_period, board->seed + 1u)
		+ 0.005f * cell_noise(u * (float)short_period, 0.61f, short_period, board->seed + 2u) - 0.0025f * smooth_step(0.25f, 0.7f, chips));
}

// The boards: each one's wood, its knots, checks, nails and lichens
static bool weatherboard_lay(weatherboard_wall_t *wall, const texture_settings_t *settings, float meters)
{
	int stud_count = (int)fmaxf(roundf(meters / 0.9f), 1.0f);
	int patch_period = (int)fmaxf(roundf(meters / 0.25f), 2.0f);

	wall->meters = meters;
	wall->seed = settings->seed;
	wall->lichen = settings->moss >= 0.0f ? settings->moss : 0.4f;
	wall->count = (int)fmaxf(roundf(meters / settings->board_meters), 1.0f);
	wall->board_height = meters / (float)wall->count;
	wall->boards = calloc((size_t)wall->count, sizeof(weatherboard_board_t));
	if (wall->boards == NULL)
	{
		fprintf(stderr, "potop_texture: out of memory\n");
		return false;
	}
	for (int b = 0; b < wall->count; b++)
	{
		weatherboard_board_t *board = &wall->boards[b];
		uint32_t seed = hash_u32(settings->seed * 7919u + (uint32_t)b * 104729u + 17u);
		float half = wall->board_height * 0.5f;
		int knots;
		int checks;

		board->seed = seed;
		board->tone = 0.86f + 0.28f * hash_unit(0, 0, seed);
		board->gray = 0.15f + 0.5f * hash_unit(1, 0, seed);
		board->warm = hash_unit(2, 0, seed) * 2.0f - 1.0f;
		board->ring_spacing = 0.006f + 0.005f * hash_unit(3, 0, seed);
		// (the pith near the board's middle: arches; far off it: straight
		// grain, as a board cut near the log's edge or across its rings)
		board->pith_across = (hash_unit(4, 0, seed) * 2.0f - 1.0f) * half * (hash_unit(5, 0, seed) < 0.6f ? 0.7f : 2.5f);
		board->pith_depth = 0.05f + 0.16f * hash_unit(6, 0, seed);
		board->pith_wander = 0.01f + 0.03f * hash_unit(7, 0, seed);
		board->waviness = 0.55f + 0.75f * hash_unit(8, 0, seed);
		// Knots: one every meter or so, most boards with one or two
		knots = (int)(meters * (0.4f + 1.1f * hash_unit(9, 0, seed)) + hash_unit(10, 0, seed));
		for (int k = 0; k < knots && board->knot_count < WEATHERBOARD_KNOTS; k++)
		{
			weatherboard_knot_t *knot = &board->knots[board->knot_count];
			bool clear = true;

			knot->radius = fminf(0.009f + 0.015f * powf(hash_unit(k, 11, seed), 1.5f), half * 0.3f);
			knot->x = hash_unit(k, 12, seed) * meters;
			knot->y = (hash_unit(k, 13, seed) * 2.0f - 1.0f) * (half - knot->radius * 2.2f) * 0.8f;
			knot->turn = hash_unit(k, 14, seed) * 2.0f * PI_F;
			knot->dead = hash_unit(k, 15, seed) < 0.3f ? 0.5f + 0.5f * hash_unit(k, 16, seed) : 0.3f * hash_unit(k, 16, seed);
			// (not on top of another one)
			for (int other = 0; other < board->knot_count; other++)
			{
				if (fabsf(wrap_difference(board->knots[other].x, knot->x, meters)) < 0.12f)
				{
					clear = false;
				}
			}
			if (clear)
			{
				board->knot_count++;
			}
		}
		// Checks: long thin ones along the grain, more of them in a board
		// weathered gray
		checks = (int)(meters * (1.2f + 2.6f * board->gray) + hash_unit(20, 0, seed));
		for (int k = 0; k < checks && board->check_count < WEATHERBOARD_CHECKS; k++)
		{
			weatherboard_check_t *check = &board->checks[board->check_count++];

			check->x = hash_unit(k, 21, seed) * meters;
			check->level = (hash_unit(k, 22, seed) * 2.0f - 1.0f) * half * 0.85f;
			check->half_length = fminf(0.04f + 0.3f * powf(hash_unit(k, 23, seed), 2.0f), meters * 0.4f);
			check->width = (0.0007f + 0.0016f * powf(hash_unit(k, 24, seed), 2.0f)) * fminf(check->half_length / 0.08f + 0.4f, 1.0f);
		}
		// Nails: one at each stud (the studs a little off plumb from board
		// to board: driven by eye), a few lost or sunk out of sight
		for (int s = 0; s < stud_count && board->nail_count < WEATHERBOARD_NAILS; s++)
		{
			weatherboard_nail_t *nail;

			if (hash_unit(s, 30 + b, settings->seed + 77u) < 0.3f)
			{
				continue;
			}
			nail = &board->nails[board->nail_count++];
			nail->x = ((float)s + 0.31f) / (float)stud_count * meters + (hash_unit(s, 31, seed) - 0.5f) * 0.03f;
			nail->y = wall->board_height * (0.3f + 0.3f * hash_unit(s, 32, seed));
			nail->radius = 0.007f + 0.0025f * hash_unit(s, 33, seed);
			nail->streak = 0.05f + 0.07f * hash_unit(s, 34, seed);
		}
		// Lichen rosettes: along the lower edge, in colonies (many where a
		// patch of them has taken, none between)
		{
			int tries = (int)(meters * 170.0f * wall->lichen);

			for (int k = 0; k < tries && board->lichen_count < WEATHERBOARD_LICHENS; k++)
			{
				float x = hash_unit(k, 40, seed) * meters;
				float patch = cell_noise(x / meters * (float)patch_period, 0.5f, patch_period, seed + 41u)
					+ 0.5f * cell_noise(x / meters * (float)(patch_period * 3), 0.2f, patch_period * 3, seed + 42u);
				float size = powf(hash_unit(k, 43, seed), 2.2f);
				weatherboard_lichen_t *lichen;

				if (hash_unit(k, 44, seed) > smooth_step(0.05f, 0.55f, patch + (wall->lichen - 0.4f)))
				{
					continue;
				}
				lichen = &board->lichens[board->lichen_count++];
				lichen->x = x;
				lichen->radius = 0.005f + 0.016f * size;
				// (the big ones sit on the edge itself, the small ones
				// scattered a little way up from it)
				lichen->y = weatherboard_edge(wall, b, x) - (float)b * wall->board_height - lichen->radius * 0.9f
					- 0.03f * powf(hash_unit(k, 45, seed), 3.0f) * (1.0f - size);
				lichen->lobes = 5.0f + floorf(hash_unit(k, 46, seed) * 4.0f);
				lichen->turn = hash_unit(k, 47, seed) * 2.0f * PI_F;
				lichen->yellow = 0.6f * powf(hash_unit(k, 48, seed), 2.0f);
			}
		}
	}
	return true;
}

// The wall at a point: its height (meters), color and roughness
static void weatherboard_sample(const weatherboard_wall_t *wall, const float wood[3], float x, float y, float *height_out, float color_out[3],
	float *roughness_out)
{
	float meters = wall->meters;
	float board_height = wall->board_height;
	float u = x / meters;
	float v = y / meters;
	int number = (int)floorf(y / board_height) - 1;
	const weatherboard_board_t *board;
	float bottom = weatherboard_edge(wall, number, x);
	float top;
	float down;
	float up;
	float local;
	float across;
	float grain;
	float knot_body = 0.0f;
	float knot_rim = 0.0f;
	float knot_rings = 0.0f;
	float knot_star = 0.0f;
	float knot_dead = 0.0f;
	float height;
	float color[3];
	float roughness;

	// Which board: the first whose lower edge is under the point
	while (y > bottom)
	{
		number++;
		bottom = weatherboard_edge(wall, number, x);
	}
	top = weatherboard_edge(wall, number - 1, x);
	board = &wall->boards[wrap_index(number, wall->count)];
	down = y - top;
	up = bottom - y;
	local = y - (float)number * board_height;
	across = local - board_height * 0.5f;
	// The grain's lines: the board's own across, pushed aside round each
	// knot as a stream round a stone (further along the board than across
	// it); in the knot its own rings, a dark gap round a dead one, checks
	// from its middle
	grain = across;
	for (int k = 0; k < board->knot_count; k++)
	{
		const weatherboard_knot_t *knot = &board->knots[k];
		float dx = wrap_difference(knot->x, x, meters);
		float dy = across - knot->y;
		float reach = knot->radius * 2.2f;
		float along = dx / 2.8f;
		float window = 1.0f - smooth_step(0.2f * meters, 0.45f * meters, fabsf(dx));
		float from = sqrtf(dx * dx / (1.25f * 1.25f) + dy * dy) / knot->radius;

		grain -= dy * reach * reach / (along * along + dy * dy + reach * reach) * window;
		if (from < 1.4f)
		{
			float angle = atan2f(dy, dx) + knot->turn;
			float lumpy = cell_noise(dx / knot->radius * 1.7f + 9.0f, dy / knot->radius * 1.7f + 9.0f, 64, board->seed + 50u + (uint32_t)k);
			float wobbled = from * (1.0f + 0.03f * sinf(2.0f * angle) + 0.1f * lumpy);
			float body = 1.0f - smooth_step(0.7f, 1.0f, wobbled);
			float rim = expf(-powf((wobbled - 1.0f) / 0.07f, 2.0f));
			float rings = 0.5f + 0.5f * cosf(2.0f * PI_F * wobbled * knot->radius / 0.0022f);
			float star = 0.0f;

			for (int j = 0; j < 3; j++)
			{
				float ray = angle - (float)j * 2.1f - 0.5f * sinf(knot->turn * 7.0f + (float)j);
				float width = 0.0008f * (1.0f - 0.7f * from);

				if (cosf(ray) > 0.0f && width > 0.0f)
				{
					star = fmaxf(star, (1.0f - smooth_step(0.4f * width, width, from * knot->radius * fabsf(sinf(ray))))
						* smooth_step(0.03f, 0.15f, from) * (1.0f - smooth_step(0.55f, 0.9f, from)));
				}
			}
			if (body > knot_body)
			{
				knot_body = body;
				knot_rings = rings;
				knot_dead = knot->dead;
			}
			knot_rim = fmaxf(knot_rim, rim * (0.35f + 0.65f * knot->dead));
			knot_star = fmaxf(knot_star, star * (0.4f + 0.6f * knot->dead));
		}
	}
	{
		// The rings: how far from the log's pith (it wanders under the
		// board), a furrow every ring_spacing of it (the rings not all
		// as thick), the lines a little ragged
		int depth_period = (int)fmaxf(roundf(meters / 0.8f), 1.0f);
		int ragged_period = (int)fmaxf(roundf(meters / 0.06f), 4.0f);
		float depth = fabsf(board->pith_depth + board->pith_wander * (cell_noise(u * (float)depth_period, 0.3f, depth_period, board->seed + 11u)
			+ 0.2f * cell_noise(u * (float)(depth_period * 3), 0.7f, depth_period * 3, board->seed + 12u))) + 0.004f;
		float waved = grain + 0.004f * cell_noise(u * (float)(depth_period * 4), across / 0.06f + 30.0f, depth_period * 4, board->seed + 15u);
		float ring_radius = sqrtf((waved - board->pith_across) * (waved - board->pith_across) + depth * depth);
		float phase = ring_radius / board->ring_spacing + 1.3f * cell_noise(ring_radius * 45.0f, 3.3f, 1024, board->seed + 13u)
			+ 0.15f * cell_noise(u * (float)ragged_period, grain / 0.008f + 50.0f, ragged_period, board->seed + 14u);
		float within = phase - floorf(phase);
		// (finer lines between the furrows: the thin rings of bad years)
		float fine_phase = phase * 3.0f + 0.5f * cell_noise(u * (float)ragged_period * 2.0f, grain / 0.004f + 80.0f, ragged_period * 2, board->seed + 16u);
		float fine_line = 0.5f + 0.5f * cosf(2.0f * PI_F * fine_phase);
		float grit = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.003f), 16.0f), 2, 0.6f, board->seed + 17u);
		// (the hard late wood stands, the soft early wood is worn away: up
		// slowly, down at once)
		float ridge = smooth_step(0.0f, 0.7f, within) * (1.0f - smooth_step(0.8f, 1.0f, within));
		float late = smooth_step(0.55f, 0.75f, within) * (1.0f - smooth_step(0.85f, 1.0f, within));
		// (the furrow itself: narrow, dark, a millimeter or so wide however
		// far apart the lines are here: they spread where the cut runs
		// along a ring)
		float line_spacing = board->ring_spacing * ring_radius / fmaxf(fabsf(waved - board->pith_across), 1e-4f);
		float furrow_width = fminf(fmaxf(0.0011f / line_spacing, 0.015f), 0.2f);
		float furrow = 1.0f - smooth_step(0.3f * furrow_width, furrow_width, fminf(within, 1.0f - within));
		// Its fibers (fine lines along the grain), long streaks of lighter
		// and darker wood, hairline checks
		float fiber_v = (y + grain - across) / meters;
		float fiber = stretched_noise(u, fiber_v, (int)fmaxf(roundf(meters / 0.025f), 8.0f), (int)fmaxf(roundf(meters / 0.0014f), 16.0f), 2,
			board->seed + 21u);
		float streak = stretched_noise(u, fiber_v, (int)fmaxf(roundf(meters / 0.3f), 2.0f), (int)fmaxf(roundf(meters / 0.009f), 8.0f), 3,
			board->seed + 22u);
		float hairline = smooth_step(0.42f, 0.56f, stretched_noise(u, fiber_v, (int)fmaxf(roundf(meters / 0.09f), 4.0f),
			(int)fmaxf(roundf(meters / 0.0028f), 16.0f), 2, board->seed + 23u));
		// (thin dark lines where the fibers have parted: along the grain)
		float parted = 1.0f - smooth_step(0.0f, 0.1f, fabsf(stretched_noise(u, fiber_v, (int)fmaxf(roundf(meters / 0.07f), 4.0f),
			(int)fmaxf(roundf(meters / 0.0022f), 16.0f), 2, board->seed + 26u)));
		float patches = tiling_fractal(u, fiber_v, (int)fmaxf(roundf(meters / 0.3f), 2.0f), 3, 0.55f, board->seed + 24u);
		float cup = cell_noise(u * (float)depth_period * 2.0f, across / 0.08f + 20.0f, depth_period * 2, board->seed + 25u);
		float check = 0.0f;
		float weathered;
		float tone;
		float luminance;

		// Checks: each along its own line of the grain (so round the
		// knots with it), widest at its middle, drawn out to nothing
		for (int k = 0; k < board->check_count; k++)
		{
			const weatherboard_check_t *crack = &board->checks[k];
			float dx = wrap_difference(crack->x, x, meters);
			float taper;
			float width;

			if (fabsf(dx) >= crack->half_length)
			{
				continue;
			}
			taper = 1.0f - (dx / crack->half_length) * (dx / crack->half_length);
			width = crack->width * sqrtf(taper);
			check = fmaxf(check, 1.0f - smooth_step(0.3f * width, width, fabsf(grain - crack->level + 0.002f * streak)));
		}
		// Its height: the face leaning out to the lower edge, that edge
		// worn round; a little cupped; furrows, fibers, checks; a knot
		// standing proud of the worn wood round it
		height = 0.011f * local / board_height + 0.0012f * cup + (0.0004f * ridge - 0.0008f * furrow + 0.0002f * fine_line - 0.0002f * parted) * (1.0f - knot_body)
			+ 0.0003f * fiber
			+ 0.0004f * streak + 0.00012f * grit
			- 0.003f * check - 0.0005f * hairline * (1.0f - knot_body)
			+ knot_body * (0.0006f + 0.0003f * knot_rings) - 0.0014f * knot_rim - 0.0012f * knot_star;
		if (up < 0.0035f)
		{
			float in = 1.0f - up / 0.0035f;

			height -= 0.003f * (1.0f - sqrtf(fmaxf(1.0f - in * in, 0.0f)));
		}
		// Its color: the wood's, the furrows dark and the late wood a
		// little darker than the ridge's top; grayer where it has
		// weathered (the ridges first); dirtier up under the board above
		tone = board->tone * (1.0f + 0.16f * streak) * (1.0f + 0.13f * fiber) * (0.98f + 0.1f * ridge - 0.06f * late - 0.5f * furrow * (1.0f - knot_body))
			* (0.93f + 0.12f * fine_line) * (1.0f - 0.24f * parted * (1.0f - knot_body)) * (1.0f + 0.14f * grit) * (1.0f - 0.35f * hairline) * (0.84f + 0.2f * smooth_step(0.0f, board_height * 0.5f, down))
			* (1.0f + 0.3f * expf(-up / 0.0025f));
		color[0] = wood[0] * tone * (1.0f + 0.05f * board->warm + 0.04f * streak);
		color[1] = wood[1] * tone;
		color[2] = wood[2] * tone * (1.0f - 0.07f * board->warm - 0.05f * streak);
		weathered = board->gray * smooth_step(-0.35f, 0.45f, patches + 0.35f * ridge - 0.1f);
		luminance = color[0] * 0.3f + color[1] * 0.59f + color[2] * 0.11f;
		color[0] = mix_float(color[0], luminance * 1.14f, weathered);
		color[1] = mix_float(color[1], luminance * 1.13f, weathered);
		color[2] = mix_float(color[2], luminance * 1.10f, weathered);
		roughness = 0.86f - 0.06f * ridge + 0.04f * fiber;
		// The knot: darker, redder, its rings close; a dead one nearly
		// black, the gap round it and its checks black
		if (knot_body > 0.0f || knot_rim > 0.0f)
		{
			float dark = mix_float(0.74f, 0.4f, knot_dead) * (0.8f + 0.3f * knot_rings) * (1.0f + 0.15f * fiber + 0.15f * grit);
			float knot_color[3] = { wood[0] * dark * 1.06f, wood[1] * dark * 0.95f, wood[2] * dark * 0.86f };

			for (int k = 0; k < 3; k++)
			{
				color[k] = mix_float(color[k], knot_color[k], knot_body) * (1.0f - 0.6f * knot_rim) * (1.0f - 0.7f * knot_star);
			}
			roughness = mix_float(roughness, 0.7f, knot_body);
		}
		for (int k = 0; k < 3; k++)
		{
			color[k] *= 1.0f - 0.8f * check;
		}
		// Mold: black, where the damp runs down the wall (over board
		// after board), blotchy, soaked along the fibers
		{
			float runs = stretched_noise(u, v, (int)fmaxf(roundf(meters / 0.3f), 2.0f), (int)fmaxf(roundf(meters / 0.8f), 1.0f), 3, wall->seed + 61u);
			float blots = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.035f), 8.0f), 3, 0.6f, wall->seed + 63u);
			float mold = smooth_step(0.3f, 0.52f, runs + 0.12f * blots) * smooth_step(-0.25f, 0.3f, blots + 0.5f * fiber + 0.3f * (1.0f - ridge) - 0.15f);

			color[0] = mix_float(color[0], color[0] * 0.30f, 0.85f * mold);
			color[1] = mix_float(color[1], color[1] * 0.33f, 0.85f * mold);
			color[2] = mix_float(color[2], color[2] * 0.36f, 0.85f * mold);
			roughness = mix_float(roughness, 0.93f, mold);
		}
		// Nails: the head a little dome, hammered, sunk in the wood it
		// crushed; rust in the wood round it and run down from it (along
		// the furrows more than over the ridges)
		for (int k = 0; k < board->nail_count; k++)
		{
			const weatherboard_nail_t *nail = &board->nails[k];
			float dx = wrap_difference(nail->x, x, meters);
			float dy = local - nail->y;
			float from = sqrtf(dx * dx + dy * dy);
			float head;
			float halo;
			float run = 0.0f;

			if (fabsf(dx) > 0.03f || dy < -0.03f || dy > nail->streak)
			{
				continue;
			}
			head = 1.0f - smooth_step(nail->radius - 0.0006f, nail->radius, from);
			halo = (1.0f - smooth_step(nail->radius, nail->radius * 2.4f, from)) * 0.7f;
			if (dy > 0.0f)
			{
				float part = dy / nail->streak;
				float width = nail->radius * (0.9f + 0.5f * part);

				run = powf(1.0f - part, 1.4f) * (1.0f - smooth_step(0.35f * width, width, fabsf(dx + 0.002f * streak * part)))
					* clamp_unit(0.85f + 0.3f * furrow + 0.3f * fiber);
			}
			{
				float rust = clamp_unit(fmaxf(halo, run));
				float rust_color[3] = { 0.44f, 0.23f, 0.11f };
				float mottle = 0.5f + 0.5f * cell_noise(dx / 0.0012f + 40.0f, dy / 0.0012f + 40.0f, 256, board->seed + 70u + (uint32_t)k);
				float head_color[3] = { mix_float(0.19f, 0.40f, mottle), mix_float(0.13f, 0.21f, mottle), mix_float(0.11f, 0.11f, mottle) };
				float dome = sqrtf(fmaxf(1.0f - (from / nail->radius) * (from / nail->radius), 0.0f));

				for (int c = 0; c < 3; c++)
				{
					color[c] = mix_float(color[c], mix_float(color[c], rust_color[c], 0.8f), rust);
					color[c] = mix_float(color[c], head_color[c] * (0.6f + 0.7f * dome_light(dx / nail->radius, dy / nail->radius)), head);
				}
				height -= 0.0012f * (1.0f - smooth_step(nail->radius * 0.8f, nail->radius * 3.0f, from));
				height += head * (0.0006f + 0.0009f * dome + 0.00015f * mottle);
				roughness = mix_float(roughness, 0.78f, head);
			}
		}
		// Lichen along the lower edge: a crust of small pale dots first,
		// then the rosettes, each of lobes round its middle, a second ring
		// of them inside, their rims lifted and paler
		if (wall->lichen > 0.0f && up < 0.07f)
		{
			float edge_near = 1.0f - smooth_step(0.004f, 0.03f + 0.03f * wall->lichen, up);
			float colonies = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.2f), 2.0f), 3, 0.55f, board->seed + 81u);
			float dots = tiling_fractal(u, v, (int)fmaxf(roundf(meters / 0.006f), 16.0f), 2, 0.6f, board->seed + 83u);
			float crust = smooth_step(0.25f, 0.45f, dots + 0.9f * colonies + 1.1f * edge_near - 1.2f + 0.5f * wall->lichen) * (1.0f - 0.7f * check);
			float crust_color[3] = { 0.58f, 0.60f, 0.50f };

			for (int c = 0; c < 3; c++)
			{
				color[c] = mix_float(color[c], crust_color[c] * (0.85f + 0.2f * dots), 0.8f * crust);
			}
			height += 0.00025f * crust;
			roughness = mix_float(roughness, 0.95f, crust);
			for (int k = 0; k < board->lichen_count; k++)
			{
				const weatherboard_lichen_t *lichen = &board->lichens[k];
				float dy = local - lichen->y;
				float dx;
				float from;
				float angle;
				float petal;
				float inner_petal;
				float outline;
				float inner;
				float cover;
				float part;
				float shade;
				float lichen_color[3];

				if (fabsf(dy) > lichen->radius)
				{
					continue;
				}
				dx = wrap_difference(lichen->x, x, meters);
				if (fabsf(dx) > lichen->radius)
				{
					continue;
				}
				from = sqrtf(dx * dx + dy * dy);
				angle = atan2f(dy, dx) + lichen->turn;
				// (a lobe: wide and round at its end, a notch between it
				// and the next; uneven, no two alike)
				petal = fabsf(cosf(lichen->lobes * 0.5f * angle + 0.6f * sinf(2.0f * angle + lichen->turn)));
				inner_petal = fabsf(cosf(lichen->lobes * 0.5f * angle + 1.3f + 0.5f * sinf(3.0f * angle)));
				outline = lichen->radius * (0.62f + 0.38f * powf(petal, 0.45f)) * (0.9f + 0.1f * sinf(3.0f * angle + lichen->turn * 3.0f));
				inner = outline * (0.42f + 0.16f * powf(inner_petal, 0.5f));
				cover = 1.0f - smooth_step(outline - 0.0007f, outline, from);
				if (cover <= 0.0f)
				{
					continue;
				}
				part = from / fmaxf(outline, 1e-6f);
				// (dark in the notches between lobes and under the inner
				// ring's rim, pale on the rims, darker and browner at its
				// old middle)
				shade = (0.72f + 0.28f * smooth_step(0.0f, 0.3f, petal + (1.0f - part) * 0.6f))
					* (1.0f - 0.35f * expf(-powf((from - inner) / 0.0006f, 2.0f)) * (from > inner ? 1.0f : 0.0f))
					* (0.8f + 0.25f * smooth_step(0.55f, 0.95f, part))
					* (0.78f + 0.22f * smooth_step(0.05f, 0.3f, part)) * (0.92f + 0.12f * dots);
				lichen_color[0] = mix_float(0.63f, 0.66f, lichen->yellow) * shade;
				lichen_color[1] = mix_float(0.66f, 0.64f, lichen->yellow) * shade;
				lichen_color[2] = mix_float(0.54f, 0.36f, lichen->yellow) * shade;
				for (int c = 0; c < 3; c++)
				{
					color[c] = mix_float(color[c], lichen_color[c], cover);
				}
				height += cover * (0.0004f + 0.0005f * smooth_step(0.6f, 0.95f, part) + 0.0003f * (from < inner ? 1.0f : 0.0f));
				roughness = mix_float(roughness, 0.96f, cover);
			}
		}
		// The shade of the board above on this one's top
		{
			float shade = 1.0f - 0.6f * expf(-down / 0.007f);

			for (int c = 0; c < 3; c++)
			{
				color[c] *= shade;
			}
		}
	}
	*height_out = height;
	memcpy(color_out, color, sizeof(color));
	*roughness_out = clamp_unit(roughness);
}

static bool make_weatherboard(const texture_settings_t *settings, field_t *field)
{
	int size = field->size;
	float pixel_meters = field->meters / (float)size;
	weatherboard_wall_t wall;

	memset(&wall, 0, sizeof(wall));
	if (!weatherboard_lay(&wall, settings, field->meters))
	{
		return false;
	}
	// (each pixel the average of four points in it: the furrows, the
	// checks and the boards' edges are finer than a pixel)
#pragma omp parallel for schedule(dynamic, 8)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			int index = y * size + x;
			float height_sum = 0.0f;
			float color_sum[3] = { 0.0f, 0.0f, 0.0f };
			float roughness_sum = 0.0f;

			for (int s = 0; s < 4; s++)
			{
				float height;
				float color[3];
				float roughness;

				weatherboard_sample(&wall, settings->color, ((float)x + 0.25f + 0.5f * (float)(s & 1)) * pixel_meters,
					((float)y + 0.25f + 0.5f * (float)(s >> 1)) * pixel_meters, &height, color, &roughness);
				height_sum += height;
				roughness_sum += roughness;
				for (int k = 0; k < 3; k++)
				{
					color_sum[k] += color[k];
				}
			}
			field->height[index] = height_sum * 0.25f;
			field->roughness[index] = roughness_sum * 0.25f;
			for (int k = 0; k < 3; k++)
			{
				field->color[index * 3 + k] = color_sum[k] * 0.25f;
			}
		}
	}
	free(wall.boards);
	shade_hollows(field, (int)fminf(fmaxf(0.004f / pixel_meters, 2.0f), 8.0f), 0.004f, 0.5f);
	bake_light(field, settings->baked_light);
	match_average(field, settings->color);
	return true;
}

// ---------------------------------------------------------------------------

static void usage(void)
{
	fprintf(stderr, "potop_texture [--data DIR] [--size N] [--meters M] [--seed N] [--color R,G,B]\n"
		"              [--grain MM] [--ripples M] [--ripple-height M] [--plates M] [--boards M] [--moss A] [--baked-light A] [--puddles A] [--height]\n"
		"              sand|clay|mud|grass|rock|canyon|granite|stone|dirt|relief|wet_sand|wet_dirt|needles|leaves|trampled|petals|snow|drystone|coursed|weatherboard NAME\n");
}

int main(int argument_count, char **arguments)
{
	texture_settings_t settings;
	const char *data_directory = "data";
	const char *kind = NULL;
	const char *name = NULL;
	field_t field;
	bool ok = false;

	memset(&settings, 0, sizeof(settings));
	settings.size = 2048;
	settings.seed = 1u;
	settings.grain_millimeters = 1.0f;
	settings.ripple_spacing = 0.11f;
	settings.ripple_height = 0.008f;
	settings.plate_meters = 0.35f;
	settings.board_meters = 0.2f;
	settings.baked_light = 0.4f;
	settings.moss = -1.0f;
	settings.puddles = -1.0f;
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		if (argument[0] != '-')
		{
			if (kind == NULL)
			{
				kind = argument;
			}
			else
			{
				name = argument;
			}
			continue;
		}
		if (strcmp(argument, "--height") == 0)
		{
			settings.write_height = true;
			continue;
		}
		if (value == NULL)
		{
			usage();
			return 1;
		}
		i++;
		if (strcmp(argument, "--data") == 0) { data_directory = value; }
		else if (strcmp(argument, "--size") == 0) { settings.size = atoi(value); }
		else if (strcmp(argument, "--meters") == 0) { settings.meters = strtof(value, NULL); settings.meters_given = true; }
		else if (strcmp(argument, "--seed") == 0) { settings.seed = (uint32_t)strtoul(value, NULL, 10); }
		else if (strcmp(argument, "--ripples") == 0) { settings.ripple_spacing = strtof(value, NULL); }
		else if (strcmp(argument, "--ripple-height") == 0) { settings.ripple_height = strtof(value, NULL); }
		else if (strcmp(argument, "--grain") == 0) { settings.grain_millimeters = fmaxf(strtof(value, NULL), 0.05f); }
		else if (strcmp(argument, "--plates") == 0) { settings.plate_meters = fmaxf(strtof(value, NULL), 0.02f); }
		else if (strcmp(argument, "--boards") == 0) { settings.board_meters = fmaxf(strtof(value, NULL), 0.03f); }
		else if (strcmp(argument, "--baked-light") == 0) { settings.baked_light = clamp_unit(strtof(value, NULL)); }
		else if (strcmp(argument, "--moss") == 0) { settings.moss = clamp_unit(strtof(value, NULL)); }
		else if (strcmp(argument, "--puddles") == 0) { settings.puddles = clamp_unit(strtof(value, NULL)); }
		else if (strcmp(argument, "--color") == 0)
		{
			if (sscanf(value, "%f,%f,%f", &settings.color[0], &settings.color[1], &settings.color[2]) != 3)
			{
				fprintf(stderr, "--color R,G,B (0..1 each)\n");
				return 1;
			}
			settings.color_given = true;
		}
		else
		{
			usage();
			return 1;
		}
	}
	if (kind == NULL || name == NULL || settings.size < 64 || settings.size > 8192)
	{
		usage();
		return 1;
	}
	// Each kind's own size and color, where none are given
	{
		static const struct { const char *kind; float meters; float color[3]; } kinds[] =
		{
			{ "sand", 2.0f, { 0.78f, 0.66f, 0.48f } },
			{ "clay", 3.0f, { 0.67f, 0.56f, 0.43f } },
			{ "rock", 4.0f, { 0.40f, 0.43f, 0.46f } },
			{ "canyon", 4.0f, { 0.66f, 0.42f, 0.28f } },
			{ "granite", 4.0f, { 0.52f, 0.50f, 0.47f } },
			{ "stone", 4.0f, { 0.42f, 0.44f, 0.46f } },
			{ "relief", 96.0f, { 0.5f, 0.5f, 0.5f } },
			{ "mud", 3.0f, { 0.50f, 0.41f, 0.31f } },
			{ "grass", 2.0f, { 0.30f, 0.42f, 0.16f } },
			{ "dirt", 2.0f, { 0.38f, 0.29f, 0.21f } },
			{ "wet_sand", 2.0f, { 0.78f, 0.66f, 0.48f } },
			{ "wet_dirt", 2.0f, { 0.38f, 0.29f, 0.21f } },
			{ "needles", 2.0f, { 0.32f, 0.23f, 0.15f } },
			{ "leaves", 2.0f, { 0.40f, 0.28f, 0.15f } },
			{ "trampled", 2.0f, { 0.42f, 0.35f, 0.26f } },
			{ "petals", 2.0f, { 0.96f, 0.78f, 0.84f } },
			{ "snow", 4.0f, { 0.90f, 0.93f, 0.97f } },
			{ "drystone", 3.0f, { 0.49f, 0.446f, 0.384f } },
			{ "coursed", 3.0f, { 0.87f, 0.845f, 0.81f } },
			{ "weatherboard", 1.8f, { 0.30f, 0.245f, 0.20f } },
		};
		bool known = false;

		for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
		{
			if (strcmp(kinds[k].kind, kind) == 0)
			{
				known = true;
				if (!settings.meters_given)
				{
					settings.meters = kinds[k].meters;
				}
				if (!settings.color_given)
				{
					memcpy(settings.color, kinds[k].color, sizeof(settings.color));
				}
			}
		}
		if (!known)
		{
			fprintf(stderr, "potop_texture: no kind '%s' (sand, clay, mud, grass, rock, canyon, granite, stone, dirt, relief, wet_sand, wet_dirt, needles, leaves, trampled, petals, snow, drystone, coursed, weatherboard)\n", kind);
			return 1;
		}
	}
	if (settings.meters <= 0.0f || !field_create(&field, settings.size, settings.meters))
	{
		usage();
		return 1;
	}
	if (strcmp(kind, "sand") == 0)
	{
		ok = make_sand(&settings, &field);
	}
	else if (strcmp(kind, "wet_sand") == 0)
	{
		// (the water between the ripples, the troughs; less on flat sand)
		if (settings.puddles < 0.0f)
		{
			settings.puddles = settings.ripple_spacing > 0.0f ? 0.18f : 0.06f;
		}
		ok = make_sand(&settings, &field) && soak(&settings, &field, 0.6f);
	}
	else if (strcmp(kind, "wet_dirt") == 0)
	{
		if (settings.puddles < 0.0f)
		{
			settings.puddles = 0.12f;
		}
		ok = make_dirt(&settings, &field) && soak(&settings, &field, 0.85f);
	}
	else if (strcmp(kind, "clay") == 0)
	{
		ok = make_clay(&settings, &field);
	}
	else if (strcmp(kind, "rock") == 0)
	{
		ok = make_rock(&settings, &field);
	}
	else if (strcmp(kind, "canyon") == 0)
	{
		ok = make_canyon(&settings, &field);
	}
	else if (strcmp(kind, "stone") == 0)
	{
		// (granite's massive stone without its lichen: a rock's up close)
		if (settings.moss < 0.0f)
		{
			settings.moss = 0.0f;
		}
		ok = make_granite(&settings, &field);
	}
	else if (strcmp(kind, "grass") == 0)
	{
		ok = make_grass(&settings, &field);
	}
	else if (strcmp(kind, "mud") == 0)
	{
		ok = make_mud(&settings, &field);
	}
	else if (strcmp(kind, "relief") == 0)
	{
		ok = make_relief(&settings, &field);
	}
	else if (strcmp(kind, "granite") == 0)
	{
		ok = make_granite(&settings, &field);
	}
	else if (strcmp(kind, "needles") == 0)
	{
		ok = make_needles(&settings, &field);
	}
	else if (strcmp(kind, "leaves") == 0)
	{
		ok = make_leaves(&settings, &field);
	}
	else if (strcmp(kind, "trampled") == 0)
	{
		ok = make_trampled(&settings, &field);
	}
	else if (strcmp(kind, "petals") == 0)
	{
		ok = make_petals(&settings, &field);
	}
	else if (strcmp(kind, "snow") == 0)
	{
		ok = make_snow(&settings, &field);
	}
	else if (strcmp(kind, "drystone") == 0)
	{
		ok = make_drystone(&settings, &field);
	}
	else if (strcmp(kind, "coursed") == 0)
	{
		ok = make_coursed(&settings, &field);
	}
	else if (strcmp(kind, "weatherboard") == 0)
	{
		ok = make_weatherboard(&settings, &field);
	}
	else
	{
		ok = make_dirt(&settings, &field);
	}
	{
		bool drystone = strcmp(kind, "drystone") == 0 || strcmp(kind, "coursed") == 0 || strcmp(kind, "weatherboard") == 0;

		ok = ok && write_field(&field, data_directory, name, settings.write_height || drystone || strcmp(kind, "relief") == 0,
			strcmp(kind, "relief") != 0 && !drystone, drystone);
	}
	field_destroy(&field);
	return ok ? 0 : 1;
}
