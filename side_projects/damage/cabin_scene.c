// The cabin: see cabin_scene.h

#include "cabin_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "box3d/box3d.h"

#include "damage_events.h"
#include "damage_model.h"

#define SETTINGS_PATH "damage.ini"
#define STEP_SECONDS (1.0f / (float)DAMAGE_SCENE_FRAMES_PER_SECOND)
#define SUBSTEP_COUNT 4
#define GRAVITY 9.81f
#define PI 3.14159265358979f
#define RECENT_BLASTS_MAX 16
#define BLAST_SHOWN_FRAMES 12

// The cabin: logs of this radius (prisms of 12 sides, so a little less
// from flat to flat), walls this far from its middle, this many courses
#define LOG_RADIUS 0.15f
#define LOG_FLAT_TO_FLAT (2.0f * LOG_RADIUS * 0.9659258f)
#define CABIN_HALF_SIZE 1.5f
#define CABIN_COURSE_COUNT 8
// How far a log that runs through a corner sticks out past it
#define CABIN_OVERHANG 0.45f
// Between a log's end and the side of the log it stops against
#define CABIN_BUTT_GAP 0.005f
#define CABIN_BEAM_COUNT 3

#define BRICK_HALF_LENGTH 0.30f
#define BRICK_HALF_HEIGHT 0.15f
#define BRICK_HALF_WIDTH 0.15f
#define WALL_ROW_COUNT 8
#define WALL_ROW_BRICKS 6

#define SKY_COLOR 0x9DB7D1
#define GROUND_COLOR_FIRST 0x6E7B5A
#define GROUND_COLOR_SECOND 0x626E50
#define BLAST_COLOR 0xFF9020
#define LABEL_COLOR 0xFFFFFF

typedef struct
{
	int frame_index;                   // the frame it shows in first
	b3Vec3 position;
} blast_mark_t;

typedef struct
{
	bool is_open;
	b3WorldId world;
	damage_settings_t settings;
	int frame_index;
	blast_mark_t recent_blasts[RECENT_BLASTS_MAX];
	int blast_count;                   // all there were; the last RECENT_BLASTS_MAX are kept
} cabin_t;

static cabin_t cabin;
// (Big: kept out of the struct that is cleared at every start)
static damage_model_t model;
static bool settings_missing_was_said;

static b3Vec3 make_vector(float x, float y, float z)
{
	b3Vec3 vector = {x, y, z};

	return vector;
}

static const float *vector_floats(const b3Vec3 *vector)
{
	return (const float *)vector;
}

static void build_ground(void)
{
	b3BodyDef body_definition = b3DefaultBodyDef();
	b3ShapeDef shape_definition = b3DefaultShapeDef();
	b3BoxHull hull = b3MakeBoxHull(40.0f, 0.5f, 40.0f);
	b3BodyId body;

	body_definition.type = b3_staticBody;
	body_definition.position = make_vector(0.0f, -0.5f, 0.0f);
	body = b3CreateBody(cabin.world, &body_definition);
	shape_definition.baseMaterial.friction = 0.7f;
	shape_definition.enableHitEvents = true;
	b3CreateHullShape(body, &shape_definition, &hull.base);
}

// The log cabin. Walls 0 and 1 are the ones along x (at +z and -z), 2
// and 3 the ones along z (at +x and -x). In an even course the logs
// along x run through the corners and those along z stop against them;
// in an odd course the other way round.
static void build_cabin(float center_x, float center_z)
{
	b3Quat along_x = b3Quat_identity;
	b3Quat along_z = b3MakeQuatFromAxisAngle(make_vector(0.0f, 1.0f, 0.0f), PI * 0.5f);
	float through_half_length = CABIN_HALF_SIZE + CABIN_OVERHANG;
	float butt_half_length = CABIN_HALF_SIZE - LOG_RADIUS - CABIN_BUTT_GAP;
	int logs[CABIN_COURSE_COUNT][4];
	int course;
	int wall;
	int side;
	int side_x;
	int side_z;
	int beam;

	for (course = 0; course < CABIN_COURSE_COUNT; course++)
	{
		float y = LOG_FLAT_TO_FLAT * (0.5f + (float)course);
		bool x_runs_through = (course & 1) == 0;
		uint32_t color = (course & 1) ? 0x8A5A30 : 0x9C6B3C;

		for (side = 0; side < 2; side++)
		{
			float offset = side == 0 ? CABIN_HALF_SIZE : -CABIN_HALF_SIZE;

			logs[course][side] = damage_model_add_log(&model, make_vector(center_x, y, center_z + offset), along_x, LOG_RADIUS,
				x_runs_through ? through_half_length : butt_half_length, DAMAGE_MATERIAL_WOOD, color);
			logs[course][2 + side] = damage_model_add_log(&model, make_vector(center_x + offset, y, center_z), along_z, LOG_RADIUS,
				x_runs_through ? butt_half_length : through_half_length, DAMAGE_MATERIAL_WOOD, color);
		}
	}

	for (course = 0; course < CABIN_COURSE_COUNT; course++)
	{
		float y = LOG_FLAT_TO_FLAT * (0.5f + (float)course);
		float y_under = LOG_FLAT_TO_FLAT * (float)course;
		bool x_runs_through = (course & 1) == 0;

		// Each log to the one under it in its own wall (to the ground in
		// the first course): near both its ends
		for (wall = 0; wall < 4; wall++)
		{
			for (side = -1; side <= 1; side += 2)
			{
				float along = (float)side * butt_half_length * 0.6f;
				float wall_offset = (wall & 1) ? -CABIN_HALF_SIZE : CABIN_HALF_SIZE;
				b3Vec3 point = wall < 2 ? make_vector(center_x + along, y_under, center_z + wall_offset)
					: make_vector(center_x + wall_offset, y_under, center_z + along);

				if (course == 0)
				{
					damage_model_add_link(&model, logs[course][wall], DAMAGE_GROUND, point, DAMAGE_LINK_FOUNDATION);
				}
				else
				{
					damage_model_add_link(&model, logs[course][wall], logs[course - 1][wall], point, DAMAGE_LINK_NAILS);
				}
			}
		}

		// At each corner: the log that stops there to the side of the one
		// running through; and the one running through to the one that ran
		// through the course under, which it lies across
		for (side_x = 0; side_x < 2; side_x++)
		{
			for (side_z = 0; side_z < 2; side_z++)
			{
				float corner_x = side_x == 0 ? CABIN_HALF_SIZE : -CABIN_HALF_SIZE;
				float corner_z = side_z == 0 ? CABIN_HALF_SIZE : -CABIN_HALF_SIZE;
				float inward_x = side_x == 0 ? -LOG_RADIUS : LOG_RADIUS;
				float inward_z = side_z == 0 ? -LOG_RADIUS : LOG_RADIUS;
				int along_x_log = logs[course][side_z];
				int along_z_log = logs[course][2 + side_x];

				if (x_runs_through)
				{
					damage_model_add_link(&model, along_z_log, along_x_log, make_vector(center_x + corner_x, y, center_z + corner_z + inward_z), DAMAGE_LINK_NAILS);
				}
				else
				{
					damage_model_add_link(&model, along_x_log, along_z_log, make_vector(center_x + corner_x + inward_x, y, center_z + corner_z), DAMAGE_LINK_NAILS);
				}
				if (course > 0)
				{
					int through_log = x_runs_through ? along_x_log : along_z_log;
					int through_log_under = x_runs_through ? logs[course - 1][2 + side_x] : logs[course - 1][side_z];

					damage_model_add_link(&model, through_log, through_log_under, make_vector(center_x + corner_x, y_under, center_z + corner_z), DAMAGE_LINK_NAILS);
				}
			}
		}
	}

	// Beams across the top, from one wall along x to the other
	for (beam = 0; beam < CABIN_BEAM_COUNT; beam++)
	{
		float x = center_x + ((float)beam - (float)(CABIN_BEAM_COUNT - 1) * 0.5f) * 0.8f;
		float y = LOG_FLAT_TO_FLAT * (0.5f + (float)CABIN_COURSE_COUNT);
		int beam_log = damage_model_add_log(&model, make_vector(x, y, center_z), along_z, LOG_RADIUS, CABIN_HALF_SIZE + 0.3f, DAMAGE_MATERIAL_WOOD, 0xA87848);

		for (side = 0; side < 2; side++)
		{
			float offset = side == 0 ? CABIN_HALF_SIZE : -CABIN_HALF_SIZE;

			damage_model_add_link(&model, beam_log, logs[CABIN_COURSE_COUNT - 1][side],
				make_vector(x, LOG_FLAT_TO_FLAT * (float)CABIN_COURSE_COUNT, center_z + offset), DAMAGE_LINK_NAILS);
		}
	}
}

// A brick wall along x: each row half a brick along from the one under
// it (an odd row has a brick less). Mortar between the bricks side by
// side and between each brick and the ones it lies on.
static void build_wall(float center_x, float center_z)
{
	b3Vec3 half_extents = make_vector(BRICK_HALF_LENGTH, BRICK_HALF_HEIGHT, BRICK_HALF_WIDTH);
	int bricks[WALL_ROW_COUNT][WALL_ROW_BRICKS];
	float brick_x[WALL_ROW_COUNT][WALL_ROW_BRICKS];
	int row;
	int brick;
	int under;

	for (row = 0; row < WALL_ROW_COUNT; row++)
	{
		int brick_count = (row & 1) ? WALL_ROW_BRICKS - 1 : WALL_ROW_BRICKS;
		float y = BRICK_HALF_HEIGHT * (1.0f + 2.0f * (float)row);

		for (brick = 0; brick < WALL_ROW_BRICKS; brick++)
		{
			bricks[row][brick] = -1;
		}
		for (brick = 0; brick < brick_count; brick++)
		{
			float x = center_x + ((float)brick - (float)(WALL_ROW_BRICKS - 1) * 0.5f) * 2.0f * BRICK_HALF_LENGTH + ((row & 1) ? BRICK_HALF_LENGTH : 0.0f);

			brick_x[row][brick] = x;
			bricks[row][brick] = damage_model_add_box(&model, make_vector(x, y, center_z), b3Quat_identity, half_extents, DAMAGE_MATERIAL_BRICK,
				((row + brick) & 1) ? 0xA8503A : 0x96452F);
			if (brick > 0)
			{
				damage_model_add_link(&model, bricks[row][brick], bricks[row][brick - 1], make_vector(x - BRICK_HALF_LENGTH, y, center_z), DAMAGE_LINK_MORTAR);
			}
			if (row == 0)
			{
				damage_model_add_link(&model, bricks[row][brick], DAMAGE_GROUND, make_vector(x, 0.0f, center_z), DAMAGE_LINK_FOUNDATION);
				continue;
			}
			for (under = 0; under < WALL_ROW_BRICKS; under++)
			{
				// (Lying on it: their middles less than a brick apart)
				if (bricks[row - 1][under] >= 0 && fabsf(brick_x[row - 1][under] - x) < 2.0f * BRICK_HALF_LENGTH - 0.01f)
				{
					damage_model_add_link(&model, bricks[row][brick], bricks[row - 1][under],
						make_vector((x + brick_x[row - 1][under]) * 0.5f, y - BRICK_HALF_HEIGHT, center_z), DAMAGE_LINK_MORTAR);
				}
			}
		}
	}
}

static void build_world(void)
{
	b3WorldDef definition = b3DefaultWorldDef();

	memset(&cabin, 0, sizeof(cabin));
	// The numbers are read at every start: change the ini, start again
	damage_settings_default(&cabin.settings);
	if (!damage_settings_load(&cabin.settings, SETTINGS_PATH) && !settings_missing_was_said)
	{
		fprintf(stderr, "cabin: no %s here, the built in numbers are used\n", SETTINGS_PATH);
		settings_missing_was_said = true;
	}

	definition.gravity = make_vector(0.0f, -GRAVITY, 0.0f);
	// One thread: the same steps must give the same world every time
	definition.workerCount = 1;
	definition.enableSleep = true;
	definition.hitEventThreshold = cabin.settings.hit_speed;
	cabin.world = b3CreateWorld(&definition);
	cabin.is_open = true;

	build_ground();
	damage_model_create(&model, cabin.world, &cabin.settings);
	build_cabin(-3.0f, 0.0f);
	build_wall(3.0f, 1.0f);
	damage_model_finish(&model);
}

// The balls and blasts of the person at the keys that belong to this frame
static void set_off_events(void)
{
	int i;

	for (i = 0; i < damage_events_count(); i++)
	{
		const damage_event_t *event = damage_events_get(i);
		b3Vec3 position = make_vector(event->position[0], event->position[1], event->position[2]);

		if (event->frame_index != cabin.frame_index)
		{
			continue;
		}
		if (event->kind == DAMAGE_EVENT_BALL)
		{
			b3BodyDef body_definition = b3DefaultBodyDef();
			b3ShapeDef shape_definition = b3DefaultShapeDef();
			b3Sphere sphere;
			b3BodyId body;

			body_definition.type = b3_dynamicBody;
			body_definition.position = position;
			body = b3CreateBody(cabin.world, &body_definition);
			shape_definition.density = cabin.settings.ball_density;
			shape_definition.baseMaterial.friction = 0.6f;
			shape_definition.enableHitEvents = true;
			sphere.center = b3Vec3_zero;
			sphere.radius = cabin.settings.ball_radius;
			b3CreateSphereShape(body, &shape_definition, &sphere);
			b3Body_SetLinearVelocity(body, make_vector(event->velocity[0], event->velocity[1], event->velocity[2]));
			damage_model_add_projectile(&model, body, cabin.settings.ball_radius);
		}
		else
		{
			blast_mark_t *mark = &cabin.recent_blasts[cabin.blast_count % RECENT_BLASTS_MAX];

			damage_model_blast(&model, position, event->strength, cabin.frame_index + 1);
			mark->frame_index = cabin.frame_index + 1;
			mark->position = position;
			cabin.blast_count++;
		}
	}
}

static void step_world(void)
{
	set_off_events();
	damage_model_before_step(&model);
	b3World_Step(cabin.world, STEP_SECONDS, SUBSTEP_COUNT);
	cabin.frame_index++;
	damage_model_after_step(&model, cabin.frame_index);
}

static bool cabin_scene_open(void)
{
	build_world();
	return true;
}

static void cabin_scene_close(void)
{
	if (cabin.is_open)
	{
		b3DestroyWorld(cabin.world);
		cabin.is_open = false;
	}
}

static void cabin_scene_default_camera(soft_render_camera_t *camera)
{
	camera->eye[0] = 1.5f;
	camera->eye[1] = 3.2f;
	camera->eye[2] = 8.5f;
	camera->target[0] = -0.3f;
	camera->target[1] = 1.1f;
	camera->target[2] = 0.3f;
	camera->vertical_field_of_view_degrees = 60.0f;
	camera->near_distance = 0.05f;
}

static void cabin_scene_seek(int frame_index)
{
	if (frame_index < 0)
	{
		frame_index = 0;
	}
	if (frame_index < cabin.frame_index)
	{
		cabin_scene_close();
		build_world();
	}
	while (cabin.frame_index < frame_index)
	{
		step_world();
	}
}

static void cabin_scene_draw(soft_render_t *renderer, const soft_render_camera_t *camera, bool draw_outlines, bool draw_debug)
{
	int first_blast = cabin.blast_count > RECENT_BLASTS_MAX ? cabin.blast_count - RECENT_BLASTS_MAX : 0;
	int i;

	soft_render_begin(renderer, camera, SKY_COLOR);
	soft_render_set_object(renderer, 1);
	soft_render_ground(renderer, 0.0f, 0.0f, 0.0f, 40.0f, 1.0f, GROUND_COLOR_FIRST, GROUND_COLOR_SECOND);
	damage_model_draw_solids(&model, renderer, 2);
	if (draw_outlines)
	{
		soft_render_outlines(renderer);
	}
	damage_model_draw_marks(&model, renderer, cabin.frame_index, draw_debug);

	for (i = first_blast; i < cabin.blast_count; i++)
	{
		const blast_mark_t *mark = &cabin.recent_blasts[i % RECENT_BLASTS_MAX];
		float screen_x;
		float screen_y;

		if (mark->frame_index > cabin.frame_index || cabin.frame_index - mark->frame_index >= BLAST_SHOWN_FRAMES)
		{
			continue;
		}
		soft_render_point(renderer, vector_floats(&mark->position), 11, BLAST_COLOR);
		if (soft_render_project(renderer, vector_floats(&mark->position), &screen_x, &screen_y))
		{
			soft_render_text(renderer, (int)screen_x + 8, (int)screen_y - 4, 1, LABEL_COLOR, "BLAST");
		}
	}
}

static void cabin_scene_describe(char *text, int text_size)
{
	damage_model_describe(&model, text, text_size);
}

static uint32_t cabin_scene_checksum(void)
{
	return damage_model_checksum(&model);
}

static bool cabin_scene_cast_ray(const float origin[3], const float direction[3], float distance, float hit_point[3])
{
	b3RayResult result = b3World_CastRayClosest(cabin.world, make_vector(origin[0], origin[1], origin[2]),
		make_vector(direction[0] * distance, direction[1] * distance, direction[2] * distance), b3DefaultQueryFilter());

	if (!result.hit)
	{
		return false;
	}
	hit_point[0] = result.point.x;
	hit_point[1] = result.point.y;
	hit_point[2] = result.point.z;
	return true;
}

const damage_scene_t cabin_scene =
{
	.name = "cabin",
	.about = "the damage model: a log cabin and a mortared wall that stand until links break",
	.open = cabin_scene_open,
	.close = cabin_scene_close,
	.default_camera = cabin_scene_default_camera,
	.seek = cabin_scene_seek,
	.draw = cabin_scene_draw,
	.describe = cabin_scene_describe,
	.checksum = cabin_scene_checksum,
	.cast_ray = cabin_scene_cast_ray,
};
