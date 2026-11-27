// The soft renderer's test scene: see test_scene.h

#include "test_scene.h"

#include <math.h>
#include <stddef.h>

#define SKY_COLOR 0x9DB7D1
#define GROUND_COLOR_FIRST 0x6E7B5A
#define GROUND_COLOR_SECOND 0x626E50
#define LOG_RADIUS 0.15f
#define LOG_LENGTH 3.6f
#define LOG_SIDE_COUNT 12
#define CABIN_HALF_SIZE 1.5f
#define CABIN_COURSE_COUNT 5
#define LINK_GOOD_COLOR 0x30E040
#define LINK_BROKEN_COLOR 0xFF3020
#define LABEL_COLOR 0xFFFFFF

static const float no_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static const uint32_t wood_colors[3] = {0x9C6B3C, 0x8A5A30, 0xA87848};

static void rotation_about_axis(float axis_x, float axis_y, float axis_z, float angle_radians, float rotation[4])
{
	float length = sqrtf(axis_x * axis_x + axis_y * axis_y + axis_z * axis_z);
	float sine = sinf(angle_radians * 0.5f) / length;

	rotation[0] = axis_x * sine;
	rotation[1] = axis_y * sine;
	rotation[2] = axis_z * sine;
	rotation[3] = cosf(angle_radians * 0.5f);
}

// A name over a point of the world
static void draw_label(soft_render_t *renderer, float x, float y, float z, const char *text)
{
	float position[3] = {x, y, z};
	float screen_x;
	float screen_y;

	if (soft_render_project(renderer, position, &screen_x, &screen_y))
	{
		soft_render_text(renderer, (int)screen_x, (int)screen_y, 1, LABEL_COLOR, text);
	}
}

// Four walls of logs laid the way a log cabin's are: those along x and
// those along z take turns, half a log apart in height, so they cross
// into one another at the corners
static void draw_cabin(soft_render_t *renderer, float center_x, float center_z, uint32_t *object_id)
{
	int course;
	int side;

	for (course = 0; course < CABIN_COURSE_COUNT; course++)
	{
		for (side = -1; side <= 1; side += 2)
		{
			float along_x_height = LOG_RADIUS + (float)course * 2.0f * LOG_RADIUS;
			float along_z_height = along_x_height + LOG_RADIUS;
			float along_x_a[3] = {center_x - LOG_LENGTH * 0.5f, along_x_height, center_z + (float)side * CABIN_HALF_SIZE};
			float along_x_b[3] = {center_x + LOG_LENGTH * 0.5f, along_x_height, center_z + (float)side * CABIN_HALF_SIZE};
			float along_z_a[3] = {center_x + (float)side * CABIN_HALF_SIZE, along_z_height, center_z - LOG_LENGTH * 0.5f};
			float along_z_b[3] = {center_x + (float)side * CABIN_HALF_SIZE, along_z_height, center_z + LOG_LENGTH * 0.5f};

			soft_render_set_object(renderer, (*object_id)++);
			soft_render_cylinder(renderer, along_x_a, along_x_b, LOG_RADIUS, LOG_SIDE_COUNT, wood_colors[(course * 2) % 3]);
			soft_render_set_object(renderer, (*object_id)++);
			soft_render_cylinder(renderer, along_z_a, along_z_b, LOG_RADIUS, LOG_SIDE_COUNT, wood_colors[(course * 2 + 1) % 3]);
		}
	}
}

// What a link between two logs could be drawn as: a short line just
// outside each corner, between a course and the next, green when it
// holds and red when it is broken (one corner here, for the look of it)
static void draw_cabin_links(soft_render_t *renderer, float center_x, float center_z)
{
	int course;
	int side_x;
	int side_z;

	renderer->line_width_pixels = 3;
	for (course = 0; course < CABIN_COURSE_COUNT; course++)
	{
		for (side_x = -1; side_x <= 1; side_x += 2)
		{
			for (side_z = -1; side_z <= 1; side_z += 2)
			{
				float corner_x = center_x + (float)side_x * (CABIN_HALF_SIZE + 0.25f);
				float corner_z = center_z + (float)side_z * (CABIN_HALF_SIZE + 0.25f);
				float lower[3] = {corner_x, LOG_RADIUS + (float)course * 2.0f * LOG_RADIUS, corner_z};
				float upper[3] = {corner_x, lower[1] + LOG_RADIUS, corner_z};
				bool broken = side_x > 0 && side_z > 0 && course >= 3;

				soft_render_line(renderer, lower, upper, broken ? LINK_BROKEN_COLOR : LINK_GOOD_COLOR);
				soft_render_point(renderer, lower, 5, 0xFFE040);
			}
		}
	}
	renderer->line_width_pixels = 1;
}

// A beam lying on three evenly spaced pillars
static void draw_beam_on_pillars(soft_render_t *renderer, float center_x, float center_z, uint32_t *object_id)
{
	float pillar_half_extents[3] = {0.15f, 0.75f, 0.15f};
	float beam_half_extents[3] = {2.5f, 0.10f, 0.15f};
	float beam_center[3] = {center_x, 1.60f, center_z};
	int pillar;

	for (pillar = -1; pillar <= 1; pillar++)
	{
		float pillar_center[3] = {center_x + (float)pillar * 2.0f, 0.75f, center_z};

		soft_render_set_object(renderer, (*object_id)++);
		soft_render_box(renderer, pillar_center, no_rotation, pillar_half_extents, 0xB0A898);
	}
	soft_render_set_object(renderer, (*object_id)++);
	soft_render_box(renderer, beam_center, no_rotation, beam_half_extents, wood_colors[2]);
}

static int current_frame_index;

static bool test_scene_open(void)
{
	current_frame_index = 0;
	return true;
}

static void test_scene_close(void)
{
}

static void test_scene_seek(int frame_index)
{
	current_frame_index = frame_index;
}

static void test_scene_describe(char *text, int text_size)
{
	if (text_size > 0)
	{
		text[0] = '\0';
	}
}

static uint32_t test_scene_checksum(void)
{
	return (uint32_t)current_frame_index;
}

static void test_scene_default_camera(soft_render_camera_t *camera)
{
	camera->eye[0] = 6.5f;
	camera->eye[1] = 4.0f;
	camera->eye[2] = 9.5f;
	camera->target[0] = -0.3f;
	camera->target[1] = 0.8f;
	camera->target[2] = 0.5f;
	camera->vertical_field_of_view_degrees = 60.0f;
	camera->near_distance = 0.05f;
}

static void test_scene_draw(soft_render_t *renderer, const soft_render_camera_t *camera, bool draw_outlines, bool draw_debug)
{
	float seconds = (float)current_frame_index / (float)DAMAGE_SCENE_FRAMES_PER_SECOND;
	uint32_t object_id = 1;
	float origin[3] = {0.0f, 0.02f, 0.0f};
	float axis_x_end[3] = {1.0f, 0.02f, 0.0f};
	float axis_y_end[3] = {0.0f, 1.02f, 0.0f};
	float axis_z_end[3] = {0.0f, 0.02f, 1.0f};
	float spinning_center[3] = {-5.0f, 1.5f, 0.0f};
	float spinning_half_extents[3] = {0.8f, 0.5f, 0.3f};
	float spinning_rotation[4];
	float bouncing_center[3] = {5.0f, 0.5f + 2.5f * fabsf(sinf(seconds * 2.0f)), 0.0f};
	float crossed_first_center[3] = {-5.0f, 0.5f, 4.5f};
	float crossed_second_center[3] = {-4.6f, 0.7f, 4.7f};
	float crossed_half_extents[3] = {0.5f, 0.5f, 0.5f};
	float crossed_rotation[4];
	float leaning_log_a[3] = {3.0f, 0.15f, 3.5f};
	float leaning_log_b[3] = {4.5f, 1.9f, 5.0f};

	soft_render_begin(renderer, camera, SKY_COLOR);

	// The ground goes under and behind the eye: it is what gets clipped
	soft_render_set_object(renderer, object_id++);
	soft_render_ground(renderer, 0.0f, 0.0f, 0.0f, 16.0f, 1.0f, GROUND_COLOR_FIRST, GROUND_COLOR_SECOND);

	draw_cabin(renderer, 0.0f, -3.0f, &object_id);
	draw_beam_on_pillars(renderer, 0.0f, 4.5f, &object_id);

	// A box turning about a slanted axis
	rotation_about_axis(1.0f, 2.0f, 0.5f, seconds, spinning_rotation);
	soft_render_set_object(renderer, object_id++);
	soft_render_box(renderer, spinning_center, spinning_rotation, spinning_half_extents, 0xC04030);

	// A ball going up and down
	soft_render_set_object(renderer, object_id++);
	soft_render_sphere(renderer, bouncing_center, 0.5f, 0x3070C0);

	// Two boxes run into one another: the depth buffer's test
	rotation_about_axis(0.0f, 1.0f, 0.0f, 0.6f, crossed_rotation);
	soft_render_set_object(renderer, object_id++);
	soft_render_box(renderer, crossed_first_center, no_rotation, crossed_half_extents, 0xD0B040);
	soft_render_set_object(renderer, object_id++);
	soft_render_box(renderer, crossed_second_center, crossed_rotation, crossed_half_extents, 0x8050A0);

	// A log leaning in no axis's direction
	soft_render_set_object(renderer, object_id++);
	soft_render_cylinder(renderer, leaning_log_a, leaning_log_b, LOG_RADIUS, LOG_SIDE_COUNT, wood_colors[0]);

	if (draw_outlines)
	{
		soft_render_outlines(renderer);
	}

	// The world's axes at its origin: x red, y green, z blue, a meter each
	renderer->line_width_pixels = 2;
	soft_render_line(renderer, origin, axis_x_end, 0xFF2020);
	soft_render_line(renderer, origin, axis_y_end, 0x20FF20);
	soft_render_line(renderer, origin, axis_z_end, 0x3060FF);
	renderer->line_width_pixels = 1;
	if (draw_debug)
	{
		draw_cabin_links(renderer, 0.0f, -3.0f);
	}

	draw_label(renderer, 0.0f, 2.0f, -3.0f, "LOG CABIN");
	draw_label(renderer, 0.0f, 2.0f, 4.5f, "BEAM ON 3 PILLARS");
	draw_label(renderer, -5.0f, 2.6f, 0.0f, "SPINNING");
	draw_label(renderer, 5.0f, 3.8f, 0.0f, "BOUNCING");
	draw_label(renderer, -5.0f, 1.5f, 4.5f, "CROSSED");
	draw_label(renderer, 4.5f, 2.2f, 5.0f, "LEANING LOG");
}

const damage_scene_t test_scene =
{
	.name = "test",
	.about = "one of everything the soft renderer draws, nothing simulated",
	.open = test_scene_open,
	.close = test_scene_close,
	.default_camera = test_scene_default_camera,
	.seek = test_scene_seek,
	.draw = test_scene_draw,
	.describe = test_scene_describe,
	.checksum = test_scene_checksum,
	.cast_ray = NULL,
};
