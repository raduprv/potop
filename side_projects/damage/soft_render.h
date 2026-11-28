// Soft render: a crude software rasterizer for looking at geometry and
// physics without a graphics card (or lavapipe's minutes a frame).
//
// Flat colored triangles with a depth buffer, shaded only by which way
// each face looks (a fixed light, so a wall and a roof of one color can
// be told apart); boxes, cylinders and spheres built from them; lines
// and points for debugging (axes, links, contacts); a small built in
// font. No textures, no lights, no shadows, no anti-aliasing: it shows
// where things are, not what the game looks like.
//
// It draws into its own buffers and knows nothing of windows or files:
// the viewer shows the same pixels the headless tool saves, so Radu and
// Claude look at the very same picture (soft_render_png.h saves them).
//
// The world is right handed with y up, in meters; a rotation is a
// quaternion x, y, z, w (Box3D's b3Quat is laid out the same way, a
// b3Vec3 is three floats: both can be passed as they are).

#ifndef SOFT_RENDER_H
#define SOFT_RENDER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	float eye[3];
	float target[3];
	float vertical_field_of_view_degrees;
	float near_distance;               // nothing nearer than this is drawn
} soft_render_camera_t;

typedef struct
{
	int width;
	int height;
	uint8_t *color;                    // width * height * 4 bytes: R, G, B, A; the top row first
	float *depth;                      // 1 / distance along the view: larger is nearer, 0 is nothing
	uint32_t *object_ids;              // which object each pixel shows (soft_render_set_object), 0 is none

	// The camera of the frame being drawn (soft_render_begin)
	float eye[3];
	float camera_right[3];
	float camera_up[3];
	float camera_forward[3];
	float projection_scale_x;
	float projection_scale_y;
	float near_distance;

	float light_direction[3];          // towards the light, unit length
	uint32_t current_object_id;
	bool cull_back_faces;              // off: both sides of a triangle are drawn (the default)
	// Over 0 (to 1): triangles are mixed into the picture by so much where
	// they are in front, and leave the depth and the object ids as they
	// were (water: draw it after the solids)
	float blend_opacity;
	int line_width_pixels;             // lines and their depth test, 1 by default
	bool marks_on_top;                 // lines and points show through the solids (to see what is inside)

	// Counted since soft_render_begin
	int triangles_given;
	int triangles_drawn;
} soft_render_t;

// False when out of memory (nothing to destroy then)
bool soft_render_create(soft_render_t *renderer, int width, int height);
void soft_render_destroy(soft_render_t *renderer);

// Clears the picture to the background color (0xRRGGBB, like every color
// here) and sets the camera for what is drawn next
void soft_render_begin(soft_render_t *renderer, const soft_render_camera_t *camera, uint32_t background_color);

// The object the next solids belong to: soft_render_outlines draws a dark
// line wherever two different objects meet on the screen. Any number but
// 0 (the background).
void soft_render_set_object(soft_render_t *renderer, uint32_t object_id);

// Solids, in world space. Triangles are counter-clockwise seen from
// outside (it only matters with cull_back_faces).
void soft_render_triangle(soft_render_t *renderer, const float corner_a[3], const float corner_b[3], const float corner_c[3], uint32_t color);
void soft_render_box(soft_render_t *renderer, const float center[3], const float rotation[4], const float half_extents[3], uint32_t color);
// A capped cylinder between two points, with this many flat sides
void soft_render_cylinder(soft_render_t *renderer, const float end_a[3], const float end_b[3], float radius, int side_count, uint32_t color);
// The same, turned about its axis so its first corner is towards
// `across` (any direction not along the axis): for a prism whose flat
// sides must be where a collision shape's are
void soft_render_prism(soft_render_t *renderer, const float end_a[3], const float end_b[3], const float across[3], float radius, int side_count, uint32_t color);
void soft_render_sphere(soft_render_t *renderer, const float center[3], float radius, uint32_t color);
// A flat checkered square at a height, to stand things on
void soft_render_ground(soft_render_t *renderer, float center_x, float center_z, float height_y, float half_size, float tile_size, uint32_t color_a, uint32_t color_b);

// A dark line between the objects (call it after the solids)
void soft_render_outlines(soft_render_t *renderer);

// Debug marks, depth tested against the solids but never shaded. Drawn a
// little towards the eye, so a line lying on a surface shows.
void soft_render_line(soft_render_t *renderer, const float end_a[3], const float end_b[3], uint32_t color);
void soft_render_point(soft_render_t *renderer, const float position[3], int size_pixels, uint32_t color);

// Where a world point is on the screen; false when it is behind the eye
bool soft_render_project(const soft_render_t *renderer, const float position[3], float *screen_x, float *screen_y);

// Text over the picture (no depth): capitals, digits and punctuation, 5 x 7
// pixels a letter times the scale, the top left corner at x, y
void soft_render_text(soft_render_t *renderer, int x, int y, int scale, uint32_t color, const char *text);

// A point of a body in world space: rotated, then moved
void soft_render_transform_point(const float body_position[3], const float body_rotation[4], const float local_point[3], float world_point[3]);

#endif
