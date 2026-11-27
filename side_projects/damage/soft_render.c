// Soft render: see soft_render.h

#include "soft_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// A triangle is clipped only when it reaches this many half screens from
// the middle of the picture (or the near distance): most are drawn as
// they are, and what is clipped stays small enough for the integer edges
#define GUARD_BAND 4.0f
// Corners snap to a sixteenth of a pixel and the edges are worked out in
// integers, so two triangles sharing an edge share it exactly (no holes
// between them, no pixel drawn twice)
#define SUBPIXEL_BITS 4
#define SUBPIXEL_ONE (1 << SUBPIXEL_BITS)
#define CLIPPED_CORNERS_MAX 12
#define CLIP_PLANE_COUNT 5
// Lines and points are drawn this much nearer than they are
#define MARK_DEPTH_BIAS 1.01f
#define AMBIENT_BRIGHTNESS 0.30f
#define OUTLINE_BRIGHTNESS 0.25f
#define SPHERE_STACK_COUNT 8
#define SPHERE_SLICE_COUNT 12
#define CYLINDER_SIDES_MAX 64
#define FONT_FIRST_CHARACTER 32
#define FONT_GLYPH_COUNT 64
#define FONT_GLYPH_WIDTH 5
#define FONT_GLYPH_HEIGHT 7
#define PI 3.14159265358979f

// A corner before the divide: x and y scaled so the picture's edges are at
// -w and w, w the distance along the view
typedef struct
{
	float x;
	float y;
	float w;
} clip_corner_t;

// A corner on the picture, in pixels (y down)
typedef struct
{
	float x;
	float y;
	float inverse_w;
} screen_corner_t;

// The classic 5 x 7 dot matrix letters, space to underscore; a row a
// byte, the leftmost dot its bit 4
static const uint8_t font_glyphs[FONT_GLYPH_COUNT][FONT_GLYPH_HEIGHT] =
{
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // space
	{0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, // !
	{0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}, // "
	{0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, // #
	{0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}, // $
	{0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, // %
	{0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}, // &
	{0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}, // '
	{0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, // (
	{0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, // )
	{0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}, // *
	{0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}, // +
	{0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}, // ,
	{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, // -
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, // .
	{0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, // /
	{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
	{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
	{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
	{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
	{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
	{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
	{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
	{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
	{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
	{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
	{0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, // :
	{0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}, // ;
	{0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, // <
	{0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}, // =
	{0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}, // >
	{0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, // ?
	{0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E}, // @
	{0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, // A
	{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
	{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
	{0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, // D
	{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
	{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
	{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, // G
	{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
	{0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
	{0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, // J
	{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
	{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
	{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
	{0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, // N
	{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
	{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
	{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
	{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
	{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
	{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
	{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
	{0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
	{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, // W
	{0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
	{0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, // Y
	{0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // Z
	{0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}, // [
	{0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}, // backslash
	{0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}, // ]
	{0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}, // ^
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}, // _
};

static void vector_subtract(const float first[3], const float second[3], float result[3])
{
	result[0] = first[0] - second[0];
	result[1] = first[1] - second[1];
	result[2] = first[2] - second[2];
}

static float vector_dot(const float first[3], const float second[3])
{
	return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

static void vector_cross(const float first[3], const float second[3], float result[3])
{
	float x = first[1] * second[2] - first[2] * second[1];
	float y = first[2] * second[0] - first[0] * second[2];
	float z = first[0] * second[1] - first[1] * second[0];

	result[0] = x;
	result[1] = y;
	result[2] = z;
}

// False (and the vector left alone) when it has no length to speak of
static bool vector_normalize(float vector[3])
{
	float length = sqrtf(vector_dot(vector, vector));

	if (length < 1e-12f)
	{
		return false;
	}
	vector[0] /= length;
	vector[1] /= length;
	vector[2] /= length;
	return true;
}

static void color_to_bytes(uint32_t color, float brightness, uint8_t bytes[3])
{
	bytes[0] = (uint8_t)((float)((color >> 16) & 0xFF) * brightness + 0.5f);
	bytes[1] = (uint8_t)((float)((color >> 8) & 0xFF) * brightness + 0.5f);
	bytes[2] = (uint8_t)((float)(color & 0xFF) * brightness + 0.5f);
}

static void world_to_clip(const soft_render_t *renderer, const float position[3], clip_corner_t *corner)
{
	float from_eye[3];

	vector_subtract(position, renderer->eye, from_eye);
	corner->x = vector_dot(from_eye, renderer->camera_right) * renderer->projection_scale_x;
	corner->y = vector_dot(from_eye, renderer->camera_up) * renderer->projection_scale_y;
	corner->w = vector_dot(from_eye, renderer->camera_forward);
}

static void clip_to_screen(const soft_render_t *renderer, const clip_corner_t *corner, screen_corner_t *screen)
{
	float inverse_w = 1.0f / corner->w;

	screen->x = (corner->x * inverse_w * 0.5f + 0.5f) * (float)renderer->width;
	screen->y = (0.5f - corner->y * inverse_w * 0.5f) * (float)renderer->height;
	screen->inverse_w = inverse_w;
}

static void clip_corner_between(const clip_corner_t *first, const clip_corner_t *second, float fraction, clip_corner_t *result)
{
	result->x = first->x + (second->x - first->x) * fraction;
	result->y = first->y + (second->y - first->y) * fraction;
	result->w = first->w + (second->w - first->w) * fraction;
}

// How far inside one of the clip planes a corner is (negative: outside).
// Plane 0 is the near distance, the other four the guard band's sides.
static float clip_plane_distance(const soft_render_t *renderer, int plane, const clip_corner_t *corner)
{
	switch (plane)
	{
		case 0:
			return corner->w - renderer->near_distance;
		case 1:
			return GUARD_BAND * corner->w + corner->x;
		case 2:
			return GUARD_BAND * corner->w - corner->x;
		case 3:
			return GUARD_BAND * corner->w + corner->y;
		default:
			return GUARD_BAND * corner->w - corner->y;
	}
}

// Cuts a polygon down to what is inside the clip planes (Sutherland and
// Hodgman's way: one plane after another). Returns how many corners are
// left, 0 when nothing is.
static int clip_polygon(const soft_render_t *renderer, clip_corner_t corners[CLIPPED_CORNERS_MAX], int corner_count)
{
	clip_corner_t clipped[CLIPPED_CORNERS_MAX];
	int plane;
	int i;

	for (plane = 0; plane < CLIP_PLANE_COUNT; plane++)
	{
		int clipped_count = 0;

		for (i = 0; i < corner_count; i++)
		{
			const clip_corner_t *current = &corners[i];
			const clip_corner_t *next = &corners[(i + 1) % corner_count];
			float current_distance = clip_plane_distance(renderer, plane, current);
			float next_distance = clip_plane_distance(renderer, plane, next);

			if (current_distance >= 0.0f)
			{
				clipped[clipped_count++] = *current;
			}
			if ((current_distance >= 0.0f) != (next_distance >= 0.0f))
			{
				float fraction = current_distance / (current_distance - next_distance);

				clip_corner_between(current, next, fraction, &clipped[clipped_count++]);
			}
		}
		if (clipped_count < 3)
		{
			return 0;
		}
		memcpy(corners, clipped, (size_t)clipped_count * sizeof(clip_corner_t));
		corner_count = clipped_count;
	}
	return corner_count;
}

static int64_t to_subpixels(float pixels)
{
	return (int64_t)lroundf(pixels * (float)SUBPIXEL_ONE);
}

// Subpixels to whole pixels, rounded down (negative ones too)
static int64_t subpixels_floor(int64_t subpixels)
{
	if (subpixels >= 0)
	{
		return subpixels >> SUBPIXEL_BITS;
	}
	return -((-subpixels + SUBPIXEL_ONE - 1) >> SUBPIXEL_BITS);
}

// Twice the area of the triangle a, b, p: positive when p is on the inner
// side of the edge from a to b, for a triangle whose own area is positive
static int64_t edge_function(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t px, int64_t py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// A pixel exactly on an edge belongs to the triangle only on its top and
// left edges (the neighbour over that edge has it as a bottom or right
// one). With y down and a positive area, a left edge goes up and a top
// edge goes right.
static bool edge_is_top_or_left(int64_t ax, int64_t ay, int64_t bx, int64_t by)
{
	int64_t delta_x = bx - ax;
	int64_t delta_y = by - ay;

	return delta_y < 0 || (delta_y == 0 && delta_x > 0);
}

static void rasterize_triangle(soft_render_t *renderer, const screen_corner_t *corner_a, const screen_corner_t *corner_b, const screen_corner_t *corner_c, const uint8_t color[3])
{
	int64_t ax = to_subpixels(corner_a->x);
	int64_t ay = to_subpixels(corner_a->y);
	int64_t bx = to_subpixels(corner_b->x);
	int64_t by = to_subpixels(corner_b->y);
	int64_t cx = to_subpixels(corner_c->x);
	int64_t cy = to_subpixels(corner_c->y);
	float inverse_w_a = corner_a->inverse_w;
	float inverse_w_b = corner_b->inverse_w;
	float inverse_w_c = corner_c->inverse_w;
	int64_t area = edge_function(ax, ay, bx, by, cx, cy);
	int64_t minimum_x;
	int64_t maximum_x;
	int64_t minimum_y;
	int64_t maximum_y;
	int64_t first_x;
	int64_t last_x;
	int64_t first_y;
	int64_t last_y;
	int64_t start_x;
	int64_t start_y;
	int64_t edge_ab_row;
	int64_t edge_bc_row;
	int64_t edge_ca_row;
	int64_t edge_ab_step_x;
	int64_t edge_bc_step_x;
	int64_t edge_ca_step_x;
	int64_t edge_ab_step_y;
	int64_t edge_bc_step_y;
	int64_t edge_ca_step_y;
	int64_t edge_ab_bias;
	int64_t edge_bc_bias;
	int64_t edge_ca_bias;
	float weight_a;
	float weight_b;
	float weight_c;
	uint32_t object_id = renderer->current_object_id;
	int64_t x;
	int64_t y;

	if (area == 0)
	{
		return;
	}
	if (area < 0)
	{
		// The other way round: swap two corners
		int64_t swapped_x = bx;
		int64_t swapped_y = by;
		float swapped_inverse_w = inverse_w_b;

		bx = cx;
		by = cy;
		inverse_w_b = inverse_w_c;
		cx = swapped_x;
		cy = swapped_y;
		inverse_w_c = swapped_inverse_w;
		area = -area;
	}

	// The pixels whose centers can be inside
	minimum_x = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
	maximum_x = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
	minimum_y = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
	maximum_y = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
	first_x = subpixels_floor(minimum_x - SUBPIXEL_ONE / 2 + SUBPIXEL_ONE - 1);
	last_x = subpixels_floor(maximum_x - SUBPIXEL_ONE / 2);
	first_y = subpixels_floor(minimum_y - SUBPIXEL_ONE / 2 + SUBPIXEL_ONE - 1);
	last_y = subpixels_floor(maximum_y - SUBPIXEL_ONE / 2);
	if (first_x < 0)
	{
		first_x = 0;
	}
	if (last_x > renderer->width - 1)
	{
		last_x = renderer->width - 1;
	}
	if (first_y < 0)
	{
		first_y = 0;
	}
	if (last_y > renderer->height - 1)
	{
		last_y = renderer->height - 1;
	}
	if (first_x > last_x || first_y > last_y)
	{
		return;
	}

	// The three edges at the first pixel's center, and how they change a
	// pixel to the right and a pixel down
	start_x = first_x * SUBPIXEL_ONE + SUBPIXEL_ONE / 2;
	start_y = first_y * SUBPIXEL_ONE + SUBPIXEL_ONE / 2;
	edge_ab_row = edge_function(ax, ay, bx, by, start_x, start_y);
	edge_bc_row = edge_function(bx, by, cx, cy, start_x, start_y);
	edge_ca_row = edge_function(cx, cy, ax, ay, start_x, start_y);
	edge_ab_step_x = -(by - ay) * SUBPIXEL_ONE;
	edge_bc_step_x = -(cy - by) * SUBPIXEL_ONE;
	edge_ca_step_x = -(ay - cy) * SUBPIXEL_ONE;
	edge_ab_step_y = (bx - ax) * SUBPIXEL_ONE;
	edge_bc_step_y = (cx - bx) * SUBPIXEL_ONE;
	edge_ca_step_y = (ax - cx) * SUBPIXEL_ONE;
	edge_ab_bias = edge_is_top_or_left(ax, ay, bx, by) ? 0 : -1;
	edge_bc_bias = edge_is_top_or_left(bx, by, cx, cy) ? 0 : -1;
	edge_ca_bias = edge_is_top_or_left(cx, cy, ax, ay) ? 0 : -1;

	// 1 / w goes in a straight line over the screen: each corner's, by how
	// near the pixel is to it (the edge opposite a corner is its weight)
	weight_a = inverse_w_a / (float)area;
	weight_b = inverse_w_b / (float)area;
	weight_c = inverse_w_c / (float)area;

	for (y = first_y; y <= last_y; y++)
	{
		int64_t edge_ab = edge_ab_row;
		int64_t edge_bc = edge_bc_row;
		int64_t edge_ca = edge_ca_row;
		int64_t pixel = y * renderer->width + first_x;

		for (x = first_x; x <= last_x; x++)
		{
			// All three not negative: their sign bits or'ed is clear
			if (((edge_ab + edge_ab_bias) | (edge_bc + edge_bc_bias) | (edge_ca + edge_ca_bias)) >= 0)
			{
				float inverse_w = (float)edge_bc * weight_a + (float)edge_ca * weight_b + (float)edge_ab * weight_c;

				if (inverse_w > renderer->depth[pixel])
				{
					uint8_t *destination = &renderer->color[pixel * 4];

					renderer->depth[pixel] = inverse_w;
					renderer->object_ids[pixel] = object_id;
					destination[0] = color[0];
					destination[1] = color[1];
					destination[2] = color[2];
				}
			}
			edge_ab += edge_ab_step_x;
			edge_bc += edge_bc_step_x;
			edge_ca += edge_ca_step_x;
			pixel++;
		}
		edge_ab_row += edge_ab_step_y;
		edge_bc_row += edge_bc_step_y;
		edge_ca_row += edge_ca_step_y;
	}
}

bool soft_render_create(soft_render_t *renderer, int width, int height)
{
	size_t pixel_count = (size_t)width * (size_t)height;

	memset(renderer, 0, sizeof(*renderer));
	renderer->width = width;
	renderer->height = height;
	renderer->color = malloc(pixel_count * 4);
	renderer->depth = malloc(pixel_count * sizeof(float));
	renderer->object_ids = malloc(pixel_count * sizeof(uint32_t));
	if (renderer->color == NULL || renderer->depth == NULL || renderer->object_ids == NULL)
	{
		soft_render_destroy(renderer);
		return false;
	}
	// From above, a little to the side and from the front: a box's top
	// and its sides all come out different
	renderer->light_direction[0] = 0.45f;
	renderer->light_direction[1] = 0.80f;
	renderer->light_direction[2] = 0.40f;
	vector_normalize(renderer->light_direction);
	renderer->line_width_pixels = 1;
	return true;
}

void soft_render_destroy(soft_render_t *renderer)
{
	free(renderer->color);
	free(renderer->depth);
	free(renderer->object_ids);
	memset(renderer, 0, sizeof(*renderer));
}

void soft_render_begin(soft_render_t *renderer, const soft_render_camera_t *camera, uint32_t background_color)
{
	size_t pixel_count = (size_t)renderer->width * (size_t)renderer->height;
	float world_up[3] = {0.0f, 1.0f, 0.0f};
	float half_angle = camera->vertical_field_of_view_degrees * 0.5f * PI / 180.0f;
	uint8_t background[3];
	size_t i;

	memcpy(renderer->eye, camera->eye, sizeof(renderer->eye));
	vector_subtract(camera->target, camera->eye, renderer->camera_forward);
	if (!vector_normalize(renderer->camera_forward))
	{
		renderer->camera_forward[0] = 0.0f;
		renderer->camera_forward[1] = 0.0f;
		renderer->camera_forward[2] = -1.0f;
	}
	vector_cross(renderer->camera_forward, world_up, renderer->camera_right);
	if (!vector_normalize(renderer->camera_right))
	{
		// Looking straight up or down: any right will do
		renderer->camera_right[0] = 1.0f;
		renderer->camera_right[1] = 0.0f;
		renderer->camera_right[2] = 0.0f;
	}
	vector_cross(renderer->camera_right, renderer->camera_forward, renderer->camera_up);
	renderer->projection_scale_y = 1.0f / tanf(half_angle);
	renderer->projection_scale_x = renderer->projection_scale_y * (float)renderer->height / (float)renderer->width;
	renderer->near_distance = camera->near_distance > 0.0f ? camera->near_distance : 0.05f;

	color_to_bytes(background_color, 1.0f, background);
	for (i = 0; i < pixel_count; i++)
	{
		renderer->color[i * 4 + 0] = background[0];
		renderer->color[i * 4 + 1] = background[1];
		renderer->color[i * 4 + 2] = background[2];
		renderer->color[i * 4 + 3] = 255;
	}
	memset(renderer->depth, 0, pixel_count * sizeof(float));
	memset(renderer->object_ids, 0, pixel_count * sizeof(uint32_t));
	renderer->current_object_id = 1;
	renderer->triangles_given = 0;
	renderer->triangles_drawn = 0;
}

void soft_render_set_object(soft_render_t *renderer, uint32_t object_id)
{
	renderer->current_object_id = object_id;
}

void soft_render_triangle(soft_render_t *renderer, const float corner_a[3], const float corner_b[3], const float corner_c[3], uint32_t color)
{
	float edge_ab[3];
	float edge_ac[3];
	float normal[3];
	float to_eye[3];
	float brightness;
	uint8_t shaded[3];
	clip_corner_t corners[CLIPPED_CORNERS_MAX];
	screen_corner_t screen[CLIPPED_CORNERS_MAX];
	int corner_count = 3;
	bool needs_clipping = false;
	int plane;
	int i;

	renderer->triangles_given++;

	// Which way it faces: the light on it, and whether we see its back
	vector_subtract(corner_b, corner_a, edge_ab);
	vector_subtract(corner_c, corner_a, edge_ac);
	vector_cross(edge_ab, edge_ac, normal);
	if (!vector_normalize(normal))
	{
		return;
	}
	vector_subtract(renderer->eye, corner_a, to_eye);
	if (vector_dot(normal, to_eye) < 0.0f)
	{
		if (renderer->cull_back_faces)
		{
			return;
		}
		// Seen from behind: lit as the side we look at
		normal[0] = -normal[0];
		normal[1] = -normal[1];
		normal[2] = -normal[2];
	}
	brightness = AMBIENT_BRIGHTNESS + (1.0f - AMBIENT_BRIGHTNESS) * (0.5f + 0.5f * vector_dot(normal, renderer->light_direction));
	color_to_bytes(color, brightness, shaded);

	world_to_clip(renderer, corner_a, &corners[0]);
	world_to_clip(renderer, corner_b, &corners[1]);
	world_to_clip(renderer, corner_c, &corners[2]);

	// All of it behind the eye or off one side of the picture: nothing
	if (corners[0].w < renderer->near_distance && corners[1].w < renderer->near_distance && corners[2].w < renderer->near_distance)
	{
		return;
	}
	if ((corners[0].x < -corners[0].w && corners[1].x < -corners[1].w && corners[2].x < -corners[2].w)
		|| (corners[0].x > corners[0].w && corners[1].x > corners[1].w && corners[2].x > corners[2].w)
		|| (corners[0].y < -corners[0].w && corners[1].y < -corners[1].w && corners[2].y < -corners[2].w)
		|| (corners[0].y > corners[0].w && corners[1].y > corners[1].w && corners[2].y > corners[2].w))
	{
		return;
	}
	for (plane = 0; plane < CLIP_PLANE_COUNT && !needs_clipping; plane++)
	{
		for (i = 0; i < 3; i++)
		{
			if (clip_plane_distance(renderer, plane, &corners[i]) < 0.0f)
			{
				needs_clipping = true;
			}
		}
	}
	if (needs_clipping)
	{
		corner_count = clip_polygon(renderer, corners, 3);
		if (corner_count == 0)
		{
			return;
		}
	}

	for (i = 0; i < corner_count; i++)
	{
		clip_to_screen(renderer, &corners[i], &screen[i]);
	}
	for (i = 1; i + 1 < corner_count; i++)
	{
		rasterize_triangle(renderer, &screen[0], &screen[i], &screen[i + 1], shaded);
	}
	renderer->triangles_drawn++;
}

static void draw_quad(soft_render_t *renderer, const float corner_a[3], const float corner_b[3], const float corner_c[3], const float corner_d[3], uint32_t color)
{
	soft_render_triangle(renderer, corner_a, corner_b, corner_c, color);
	soft_render_triangle(renderer, corner_a, corner_c, corner_d, color);
}

static void rotate_by_quaternion(const float rotation[4], const float vector[3], float result[3])
{
	float twice_cross[3];
	float second_cross[3];

	vector_cross(rotation, vector, twice_cross);
	twice_cross[0] *= 2.0f;
	twice_cross[1] *= 2.0f;
	twice_cross[2] *= 2.0f;
	vector_cross(rotation, twice_cross, second_cross);
	result[0] = vector[0] + rotation[3] * twice_cross[0] + second_cross[0];
	result[1] = vector[1] + rotation[3] * twice_cross[1] + second_cross[1];
	result[2] = vector[2] + rotation[3] * twice_cross[2] + second_cross[2];
}

void soft_render_transform_point(const float body_position[3], const float body_rotation[4], const float local_point[3], float world_point[3])
{
	float rotated[3];

	rotate_by_quaternion(body_rotation, local_point, rotated);
	world_point[0] = rotated[0] + body_position[0];
	world_point[1] = rotated[1] + body_position[1];
	world_point[2] = rotated[2] + body_position[2];
}

void soft_render_box(soft_render_t *renderer, const float center[3], const float rotation[4], const float half_extents[3], uint32_t color)
{
	// A corner's number says which side it is on: bit 0 its x, bit 1 its
	// y, bit 2 its z (set: the positive side). Each face counter-clockwise
	// seen from outside.
	static const int face_corners[6][4] =
	{
		{1, 3, 7, 5}, // +x
		{0, 4, 6, 2}, // -x
		{2, 6, 7, 3}, // +y
		{0, 1, 5, 4}, // -y
		{4, 5, 7, 6}, // +z
		{0, 2, 3, 1}, // -z
	};
	float corners[8][3];
	int i;

	for (i = 0; i < 8; i++)
	{
		float local[3];

		local[0] = (i & 1) ? half_extents[0] : -half_extents[0];
		local[1] = (i & 2) ? half_extents[1] : -half_extents[1];
		local[2] = (i & 4) ? half_extents[2] : -half_extents[2];
		soft_render_transform_point(center, rotation, local, corners[i]);
	}
	for (i = 0; i < 6; i++)
	{
		draw_quad(renderer, corners[face_corners[i][0]], corners[face_corners[i][1]], corners[face_corners[i][2]], corners[face_corners[i][3]], color);
	}
}

void soft_render_prism(soft_render_t *renderer, const float end_a[3], const float end_b[3], const float across[3], float radius, int side_count, uint32_t color)
{
	float axis[3];
	float first[3];
	float second[3];
	float along;
	float ring_a[CYLINDER_SIDES_MAX][3];
	float ring_b[CYLINDER_SIDES_MAX][3];
	int i;
	int k;

	if (side_count < 3)
	{
		side_count = 3;
	}
	if (side_count > CYLINDER_SIDES_MAX)
	{
		side_count = CYLINDER_SIDES_MAX;
	}
	vector_subtract(end_b, end_a, axis);
	if (!vector_normalize(axis))
	{
		return;
	}
	// Two directions across the axis: the one asked for, squared up to
	// the axis, and the one a quarter turn on (counter-clockwise seen
	// from end b)
	along = vector_dot(across, axis);
	for (k = 0; k < 3; k++)
	{
		first[k] = across[k] - axis[k] * along;
	}
	if (!vector_normalize(first))
	{
		return;
	}
	vector_cross(axis, first, second);

	for (i = 0; i < side_count; i++)
	{
		float angle = 2.0f * PI * (float)i / (float)side_count;
		float cosine = cosf(angle) * radius;
		float sine = sinf(angle) * radius;

		for (k = 0; k < 3; k++)
		{
			float offset = first[k] * cosine + second[k] * sine;

			ring_a[i][k] = end_a[k] + offset;
			ring_b[i][k] = end_b[k] + offset;
		}
	}
	for (i = 0; i < side_count; i++)
	{
		int next = (i + 1) % side_count;

		draw_quad(renderer, ring_a[i], ring_a[next], ring_b[next], ring_b[i], color);
		soft_render_triangle(renderer, end_b, ring_b[i], ring_b[next], color);
		soft_render_triangle(renderer, end_a, ring_a[next], ring_a[i], color);
	}
}

void soft_render_cylinder(soft_render_t *renderer, const float end_a[3], const float end_b[3], float radius, int side_count, uint32_t color)
{
	float axis[3];
	float across[3] = {0.0f, 1.0f, 0.0f};

	// Any direction that is not along the axis
	vector_subtract(end_b, end_a, axis);
	if (!vector_normalize(axis))
	{
		return;
	}
	if (fabsf(axis[1]) > 0.9f)
	{
		across[0] = 1.0f;
		across[1] = 0.0f;
	}
	soft_render_prism(renderer, end_a, end_b, across, radius, side_count, color);
}

// A point of the sphere: the stack counts down from the top, the slice
// round it
static void sphere_point(const float center[3], float radius, int stack, int slice, float point[3])
{
	float polar = PI * (float)stack / (float)SPHERE_STACK_COUNT;
	float around = 2.0f * PI * (float)slice / (float)SPHERE_SLICE_COUNT;

	point[0] = center[0] + radius * sinf(polar) * cosf(around);
	point[1] = center[1] + radius * cosf(polar);
	point[2] = center[2] + radius * sinf(polar) * sinf(around);
}

void soft_render_sphere(soft_render_t *renderer, const float center[3], float radius, uint32_t color)
{
	int stack;
	int slice;

	for (stack = 0; stack < SPHERE_STACK_COUNT; stack++)
	{
		for (slice = 0; slice < SPHERE_SLICE_COUNT; slice++)
		{
			float upper_first[3];
			float upper_second[3];
			float lower_first[3];
			float lower_second[3];

			sphere_point(center, radius, stack, slice, upper_first);
			sphere_point(center, radius, stack, slice + 1, upper_second);
			sphere_point(center, radius, stack + 1, slice, lower_first);
			sphere_point(center, radius, stack + 1, slice + 1, lower_second);
			// (At the poles one of the two has no area and is dropped)
			soft_render_triangle(renderer, upper_first, upper_second, lower_second, color);
			soft_render_triangle(renderer, upper_first, lower_second, lower_first, color);
		}
	}
}

void soft_render_ground(soft_render_t *renderer, float center_x, float center_z, float height_y, float half_size, float tile_size, uint32_t color_a, uint32_t color_b)
{
	int first_tile_x = (int)floorf((center_x - half_size) / tile_size);
	int last_tile_x = (int)ceilf((center_x + half_size) / tile_size);
	int first_tile_z = (int)floorf((center_z - half_size) / tile_size);
	int last_tile_z = (int)ceilf((center_z + half_size) / tile_size);
	int tile_x;
	int tile_z;

	// Whole tiles on the world's own grid: the pattern stays put when the
	// square moves
	for (tile_z = first_tile_z; tile_z < last_tile_z; tile_z++)
	{
		for (tile_x = first_tile_x; tile_x < last_tile_x; tile_x++)
		{
			float x0 = (float)tile_x * tile_size;
			float x1 = x0 + tile_size;
			float z0 = (float)tile_z * tile_size;
			float z1 = z0 + tile_size;
			float corner_a[3] = {x0, height_y, z0};
			float corner_b[3] = {x0, height_y, z1};
			float corner_c[3] = {x1, height_y, z1};
			float corner_d[3] = {x1, height_y, z0};

			draw_quad(renderer, corner_a, corner_b, corner_c, corner_d, ((tile_x + tile_z) & 1) ? color_b : color_a);
		}
	}
}

void soft_render_outlines(soft_render_t *renderer)
{
	int x;
	int y;

	for (y = 0; y < renderer->height; y++)
	{
		for (x = 0; x < renderer->width; x++)
		{
			int pixel = y * renderer->width + x;
			uint32_t object_id = renderer->object_ids[pixel];
			bool at_border = (x + 1 < renderer->width && renderer->object_ids[pixel + 1] != object_id)
				|| (y + 1 < renderer->height && renderer->object_ids[pixel + renderer->width] != object_id);

			if (at_border)
			{
				uint8_t *destination = &renderer->color[pixel * 4];

				destination[0] = (uint8_t)((float)destination[0] * OUTLINE_BRIGHTNESS);
				destination[1] = (uint8_t)((float)destination[1] * OUTLINE_BRIGHTNESS);
				destination[2] = (uint8_t)((float)destination[2] * OUTLINE_BRIGHTNESS);
			}
		}
	}
}

// A square of pixels round a point of the screen, where nothing nearer is
static void draw_mark(soft_render_t *renderer, float x, float y, int size_pixels, float inverse_w, const uint8_t color[3])
{
	int first_x = (int)floorf(x - 0.5f * (float)(size_pixels - 1));
	int first_y = (int)floorf(y - 0.5f * (float)(size_pixels - 1));
	float biased_inverse_w = inverse_w * MARK_DEPTH_BIAS;
	int pixel_x;
	int pixel_y;

	for (pixel_y = first_y; pixel_y < first_y + size_pixels; pixel_y++)
	{
		for (pixel_x = first_x; pixel_x < first_x + size_pixels; pixel_x++)
		{
			int pixel;

			if (pixel_x < 0 || pixel_x >= renderer->width || pixel_y < 0 || pixel_y >= renderer->height)
			{
				continue;
			}
			pixel = pixel_y * renderer->width + pixel_x;
			if (renderer->marks_on_top)
			{
				// Over everything, and leaving the depth as it was
				uint8_t *destination = &renderer->color[pixel * 4];

				destination[0] = color[0];
				destination[1] = color[1];
				destination[2] = color[2];
			}
			else if (biased_inverse_w > renderer->depth[pixel])
			{
				uint8_t *destination = &renderer->color[pixel * 4];

				renderer->depth[pixel] = biased_inverse_w;
				destination[0] = color[0];
				destination[1] = color[1];
				destination[2] = color[2];
			}
		}
	}
}

// Narrows the part of a line that is kept (Liang and Barsky's way) by one
// side of a rectangle. False when nothing is left.
static bool line_clip_side(float direction, float distance, float *first_fraction, float *last_fraction)
{
	float fraction;

	if (direction == 0.0f)
	{
		return distance >= 0.0f;
	}
	fraction = distance / direction;
	if (direction < 0.0f)
	{
		if (fraction > *last_fraction)
		{
			return false;
		}
		if (fraction > *first_fraction)
		{
			*first_fraction = fraction;
		}
	}
	else
	{
		if (fraction < *first_fraction)
		{
			return false;
		}
		if (fraction < *last_fraction)
		{
			*last_fraction = fraction;
		}
	}
	return true;
}

void soft_render_line(soft_render_t *renderer, const float end_a[3], const float end_b[3], uint32_t color)
{
	clip_corner_t clip_a;
	clip_corner_t clip_b;
	screen_corner_t screen_a;
	screen_corner_t screen_b;
	float distance_a;
	float distance_b;
	float delta_x;
	float delta_y;
	float first_fraction = 0.0f;
	float last_fraction = 1.0f;
	float margin = (float)renderer->line_width_pixels;
	float longest;
	uint8_t bytes[3];
	int step_count;
	int i;

	world_to_clip(renderer, end_a, &clip_a);
	world_to_clip(renderer, end_b, &clip_b);
	distance_a = clip_a.w - renderer->near_distance;
	distance_b = clip_b.w - renderer->near_distance;
	if (distance_a < 0.0f && distance_b < 0.0f)
	{
		return;
	}
	if (distance_a < 0.0f)
	{
		clip_corner_between(&clip_a, &clip_b, distance_a / (distance_a - distance_b), &clip_a);
	}
	else if (distance_b < 0.0f)
	{
		clip_corner_between(&clip_b, &clip_a, distance_b / (distance_b - distance_a), &clip_b);
	}
	clip_to_screen(renderer, &clip_a, &screen_a);
	clip_to_screen(renderer, &clip_b, &screen_b);

	// Only the part over the picture is walked
	delta_x = screen_b.x - screen_a.x;
	delta_y = screen_b.y - screen_a.y;
	if (!line_clip_side(-delta_x, screen_a.x + margin, &first_fraction, &last_fraction)
		|| !line_clip_side(delta_x, (float)renderer->width + margin - screen_a.x, &first_fraction, &last_fraction)
		|| !line_clip_side(-delta_y, screen_a.y + margin, &first_fraction, &last_fraction)
		|| !line_clip_side(delta_y, (float)renderer->height + margin - screen_a.y, &first_fraction, &last_fraction))
	{
		return;
	}
	longest = fabsf(delta_x) > fabsf(delta_y) ? fabsf(delta_x) : fabsf(delta_y);
	step_count = (int)ceilf(longest * (last_fraction - first_fraction)) + 1;
	color_to_bytes(color, 1.0f, bytes);
	for (i = 0; i <= step_count; i++)
	{
		float fraction = first_fraction + (last_fraction - first_fraction) * (float)i / (float)step_count;

		draw_mark(renderer, screen_a.x + delta_x * fraction, screen_a.y + delta_y * fraction, renderer->line_width_pixels,
			screen_a.inverse_w + (screen_b.inverse_w - screen_a.inverse_w) * fraction, bytes);
	}
}

void soft_render_point(soft_render_t *renderer, const float position[3], int size_pixels, uint32_t color)
{
	clip_corner_t clip;
	screen_corner_t screen;
	uint8_t bytes[3];

	world_to_clip(renderer, position, &clip);
	if (clip.w < renderer->near_distance)
	{
		return;
	}
	clip_to_screen(renderer, &clip, &screen);
	color_to_bytes(color, 1.0f, bytes);
	draw_mark(renderer, screen.x, screen.y, size_pixels, screen.inverse_w, bytes);
}

bool soft_render_project(const soft_render_t *renderer, const float position[3], float *screen_x, float *screen_y)
{
	clip_corner_t clip;
	screen_corner_t screen;

	world_to_clip(renderer, position, &clip);
	if (clip.w < renderer->near_distance)
	{
		return false;
	}
	clip_to_screen(renderer, &clip, &screen);
	*screen_x = screen.x;
	*screen_y = screen.y;
	return true;
}

static void draw_text_pass(soft_render_t *renderer, int x, int y, int scale, const uint8_t color[3], const char *text)
{
	int pen_x = x;
	int pen_y = y;
	const char *character;

	for (character = text; *character != '\0'; character++)
	{
		int code = (unsigned char)*character;
		int row;
		int column;
		int fill_x;
		int fill_y;

		if (code == '\n')
		{
			pen_x = x;
			pen_y += (FONT_GLYPH_HEIGHT + 2) * scale;
			continue;
		}
		if (code >= 'a' && code <= 'z')
		{
			code -= 'a' - 'A';
		}
		if (code < FONT_FIRST_CHARACTER || code >= FONT_FIRST_CHARACTER + FONT_GLYPH_COUNT)
		{
			code = '?';
		}
		for (row = 0; row < FONT_GLYPH_HEIGHT; row++)
		{
			uint8_t dots = font_glyphs[code - FONT_FIRST_CHARACTER][row];

			for (column = 0; column < FONT_GLYPH_WIDTH; column++)
			{
				if ((dots & (1 << (FONT_GLYPH_WIDTH - 1 - column))) == 0)
				{
					continue;
				}
				for (fill_y = pen_y + row * scale; fill_y < pen_y + (row + 1) * scale; fill_y++)
				{
					for (fill_x = pen_x + column * scale; fill_x < pen_x + (column + 1) * scale; fill_x++)
					{
						uint8_t *destination;

						if (fill_x < 0 || fill_x >= renderer->width || fill_y < 0 || fill_y >= renderer->height)
						{
							continue;
						}
						destination = &renderer->color[(fill_y * renderer->width + fill_x) * 4];
						destination[0] = color[0];
						destination[1] = color[1];
						destination[2] = color[2];
					}
				}
			}
		}
		pen_x += (FONT_GLYPH_WIDTH + 1) * scale;
	}
}

void soft_render_text(soft_render_t *renderer, int x, int y, int scale, uint32_t color, const char *text)
{
	static const uint8_t shadow[3] = {0, 0, 0};
	uint8_t bytes[3];

	if (scale < 1)
	{
		scale = 1;
	}
	// A black copy a pixel down and right first, so it reads on anything
	color_to_bytes(color, 1.0f, bytes);
	draw_text_pass(renderer, x + 1, y + 1, scale, shadow, text);
	draw_text_pass(renderer, x, y, scale, bytes, text);
}
