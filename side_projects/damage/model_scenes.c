// The scenes built on the damage model: see model_scenes.h

#include "model_scenes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "box3d/box3d.h"

#include "damage_events.h"
#include "damage_draw.h"
#include "damage_ini.h"
#include "wreck.h"

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
	wreck_settings_t settings;
	int frame_index;
	blast_mark_t recent_blasts[RECENT_BLASTS_MAX];
	int blast_count;                   // all there were; the last RECENT_BLASTS_MAX are kept
} cabin_t;

static cabin_t cabin;
// (Big: kept out of the struct that is cleared at every start)
static wreck_model_t model;
static bool settings_missing_was_said;
// Which scene's things build_world puts up
static void (*build_structures)(void);
// The piece being looked into (-1: none). It is kept through going back
// in time: the same piece can be watched over a replay.
static int inspected_piece = -1;

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
	// (Far wider than the ground that is drawn: what a blast throws
	// must come down on something, not fall for ever past the edge)
	b3BoxHull hull = b3MakeBoxHull(1000.0f, 0.5f, 1000.0f);
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

			logs[course][side] = wreck_model_add_log(&model, make_vector(center_x, y, center_z + offset), along_x, LOG_RADIUS,
				x_runs_through ? through_half_length : butt_half_length, WRECK_MATERIAL_WOOD, color);
			logs[course][2 + side] = wreck_model_add_log(&model, make_vector(center_x + offset, y, center_z), along_z, LOG_RADIUS,
				x_runs_through ? butt_half_length : through_half_length, WRECK_MATERIAL_WOOD, color);
		}
	}

	for (course = 0; course < CABIN_COURSE_COUNT; course++)
	{
		float y = LOG_FLAT_TO_FLAT * (0.5f + (float)course);
		float y_under = LOG_FLAT_TO_FLAT * (float)course;
		bool x_runs_through = (course & 1) == 0;

		// Each log to the one under it in its own wall (to the ground in
		// the first course): near both its ends. No nails in the walls:
		// a log lies in the groove of the one under and in the notches
		// of those it crosses
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
					wreck_model_add_link(&model, logs[course][wall], WRECK_GROUND, point, WRECK_LINK_FOUNDATION);
				}
				else
				{
					wreck_model_add_link(&model, logs[course][wall], logs[course - 1][wall], point, WRECK_LINK_NOTCH);
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
					wreck_model_add_link(&model, along_z_log, along_x_log, make_vector(center_x + corner_x, y, center_z + corner_z + inward_z), WRECK_LINK_NOTCH);
				}
				else
				{
					wreck_model_add_link(&model, along_x_log, along_z_log, make_vector(center_x + corner_x + inward_x, y, center_z + corner_z), WRECK_LINK_NOTCH);
				}
				if (course > 0)
				{
					int through_log = x_runs_through ? along_x_log : along_z_log;
					int through_log_under = x_runs_through ? logs[course - 1][2 + side_x] : logs[course - 1][side_z];

					wreck_model_add_link(&model, through_log, through_log_under, make_vector(center_x + corner_x, y_under, center_z + corner_z), WRECK_LINK_NOTCH);
				}
			}
		}
	}

	// Beams across the top, from one wall along x to the other
	for (beam = 0; beam < CABIN_BEAM_COUNT; beam++)
	{
		float x = center_x + ((float)beam - (float)(CABIN_BEAM_COUNT - 1) * 0.5f) * 0.8f;
		float y = LOG_FLAT_TO_FLAT * (0.5f + (float)CABIN_COURSE_COUNT);
		int beam_log = wreck_model_add_log(&model, make_vector(x, y, center_z), along_z, LOG_RADIUS, CABIN_HALF_SIZE + 0.3f, WRECK_MATERIAL_WOOD, 0xA87848);

		for (side = 0; side < 2; side++)
		{
			float offset = side == 0 ? CABIN_HALF_SIZE : -CABIN_HALF_SIZE;

			wreck_model_add_link(&model, beam_log, logs[CABIN_COURSE_COUNT - 1][side],
				make_vector(x, LOG_FLAT_TO_FLAT * (float)CABIN_COURSE_COUNT, center_z + offset), WRECK_LINK_NAILS);
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
			bricks[row][brick] = wreck_model_add_box(&model, make_vector(x, y, center_z), b3Quat_identity, half_extents, WRECK_MATERIAL_BRICK,
				((row + brick) & 1) ? 0xA8503A : 0x96452F);
			if (brick > 0)
			{
				wreck_model_add_link(&model, bricks[row][brick], bricks[row][brick - 1], make_vector(x - BRICK_HALF_LENGTH, y, center_z), WRECK_LINK_MORTAR);
			}
			if (row == 0)
			{
				wreck_model_add_link(&model, bricks[row][brick], WRECK_GROUND, make_vector(x, 0.0f, center_z), WRECK_LINK_FOUNDATION);
				continue;
			}
			for (under = 0; under < WALL_ROW_BRICKS; under++)
			{
				// (Lying on it: their middles less than a brick apart)
				if (bricks[row - 1][under] >= 0 && fabsf(brick_x[row - 1][under] - x) < 2.0f * BRICK_HALF_LENGTH - 0.01f)
				{
					wreck_model_add_link(&model, bricks[row][brick], bricks[row - 1][under],
						make_vector((x + brick_x[row - 1][under]) * 0.5f, y - BRICK_HALF_HEIGHT, center_z), WRECK_LINK_MORTAR);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------
// The support scene's three cases

// A wall of three rows on two piers of three bricks, and a pane of
// glass standing under its middle brick. The piers carry most of it;
// with one pier gone the wall balances on the pane, which has it all.
static void build_wall_on_pane(float center_x, float center_z)
{
	b3Vec3 brick_half_extents = make_vector(BRICK_HALF_LENGTH, BRICK_HALF_HEIGHT, BRICK_HALF_WIDTH);
	b3Vec3 pane_half_extents = make_vector(0.30f, 0.45f, 0.03f);
	float pier_top = 6.0f * BRICK_HALF_HEIGHT;
	int bricks[3][5];
	float brick_x[3][5];
	int pier_tops[2];
	int side;
	int row;
	int brick;
	int pane;

	// The piers, under the wall's end bricks
	for (side = -1; side <= 1; side += 2)
	{
		float x = center_x + (float)side * 4.0f * BRICK_HALF_LENGTH;
		int below = WRECK_GROUND;

		for (row = 0; row < 3; row++)
		{
			float y = BRICK_HALF_HEIGHT * (1.0f + 2.0f * (float)row);
			int pier_brick = wreck_model_add_box(&model, make_vector(x, y, center_z), b3Quat_identity, brick_half_extents, WRECK_MATERIAL_BRICK, 0x96452F);

			wreck_model_add_link(&model, pier_brick, below, make_vector(x, y - BRICK_HALF_HEIGHT, center_z), below == WRECK_GROUND ? WRECK_LINK_FOUNDATION : WRECK_LINK_MORTAR);
			below = pier_brick;
		}
		pier_tops[side < 0 ? 0 : 1] = below;
	}

	for (row = 0; row < 3; row++)
	{
		int brick_count = (row & 1) ? 4 : 5;
		float y = pier_top + BRICK_HALF_HEIGHT * (1.0f + 2.0f * (float)row);

		for (brick = 0; brick < 5; brick++)
		{
			bricks[row][brick] = -1;
		}
		for (brick = 0; brick < brick_count; brick++)
		{
			float x = center_x + ((float)brick - 2.0f) * 2.0f * BRICK_HALF_LENGTH + ((row & 1) ? BRICK_HALF_LENGTH : 0.0f);
			int below;

			brick_x[row][brick] = x;
			bricks[row][brick] = wreck_model_add_box(&model, make_vector(x, y, center_z), b3Quat_identity, brick_half_extents, WRECK_MATERIAL_BRICK,
				((row + brick) & 1) ? 0xA8503A : 0x96452F);
			if (brick > 0)
			{
				wreck_model_add_link(&model, bricks[row][brick], bricks[row][brick - 1], make_vector(x - BRICK_HALF_LENGTH, y, center_z), WRECK_LINK_MORTAR);
			}
			if (row == 0)
			{
				// The end bricks lie on the piers
				if (brick == 0 || brick == 4)
				{
					wreck_model_add_link(&model, bricks[row][brick], pier_tops[brick == 0 ? 0 : 1], make_vector(x, pier_top, center_z), WRECK_LINK_MORTAR);
				}
				continue;
			}
			for (below = 0; below < 5; below++)
			{
				if (bricks[row - 1][below] >= 0 && fabsf(brick_x[row - 1][below] - x) < 2.0f * BRICK_HALF_LENGTH - 0.01f)
				{
					wreck_model_add_link(&model, bricks[row][brick], bricks[row - 1][below],
						make_vector((x + brick_x[row - 1][below]) * 0.5f, y - BRICK_HALF_HEIGHT, center_z), WRECK_LINK_MORTAR);
				}
			}
		}
	}

	// The pane: on the ground, the wall's middle brick on it
	pane = wreck_model_add_box(&model, make_vector(center_x, 0.45f, center_z), b3Quat_identity, pane_half_extents, WRECK_MATERIAL_GLASS, 0x9FD8E8);
	wreck_model_add_link(&model, pane, WRECK_GROUND, make_vector(center_x, 0.0f, center_z), WRECK_LINK_GLAZING);
	wreck_model_add_link(&model, bricks[0][2], pane, make_vector(center_x, pier_top, center_z), WRECK_LINK_GLAZING);
}

// A wall of logs along z; three beams from its top out along x, their
// far ends on a lintel log that stands on two posts; slabs across the
// beams for weight.
static void build_roof_on_wall(float center_x, float center_z)
{
	b3Quat along_x = b3Quat_identity;
	b3Quat along_z = b3MakeQuatFromAxisAngle(make_vector(0.0f, 1.0f, 0.0f), PI * 0.5f);
	b3Quat upright = b3MakeQuatFromAxisAngle(make_vector(0.0f, 0.0f, 1.0f), PI * 0.5f);
	b3Vec3 slab_half_extents = make_vector(0.30f, 0.04f, 1.5f);
	float wall_x = center_x - 1.5f;
	float lintel_x = center_x + 1.0f;
	float wall_top = LOG_FLAT_TO_FLAT * 7.0f;
	float beam_y = wall_top + LOG_FLAT_TO_FLAT * 0.5f;
	float lintel_y = wall_top - LOG_FLAT_TO_FLAT * 0.5f;
	float post_top = wall_top - LOG_FLAT_TO_FLAT;
	int below = WRECK_GROUND;
	int beams[3];
	int lintel;
	int course;
	int side;
	int beam;
	int slab;

	for (course = 0; course < 7; course++)
	{
		float y = LOG_FLAT_TO_FLAT * (0.5f + (float)course);
		int wall_log = wreck_model_add_log(&model, make_vector(wall_x, y, center_z), along_z, LOG_RADIUS, 1.5f, WRECK_MATERIAL_WOOD, (course & 1) ? 0x8A5A30 : 0x9C6B3C);

		for (side = -1; side <= 1; side += 2)
		{
			wreck_model_add_link(&model, wall_log, below, make_vector(wall_x, LOG_FLAT_TO_FLAT * (float)course, center_z + (float)side * 0.9f),
				below == WRECK_GROUND ? WRECK_LINK_FOUNDATION : WRECK_LINK_NAILS);
		}
		below = wall_log;
	}

	lintel = wreck_model_add_log(&model, make_vector(lintel_x, lintel_y, center_z), along_z, LOG_RADIUS, 1.5f, WRECK_MATERIAL_WOOD, 0x9C6B3C);
	for (side = -1; side <= 1; side += 2)
	{
		float z = center_z + (float)side * 1.2f;
		int post = wreck_model_add_log(&model, make_vector(lintel_x, post_top * 0.5f, z), upright, LOG_RADIUS, post_top * 0.5f, WRECK_MATERIAL_WOOD, 0x8A5A30);

		wreck_model_add_link(&model, post, WRECK_GROUND, make_vector(lintel_x, 0.0f, z), WRECK_LINK_FOUNDATION);
		wreck_model_add_link(&model, lintel, post, make_vector(lintel_x, post_top, z), WRECK_LINK_NAILS);
	}

	for (beam = 0; beam < 3; beam++)
	{
		float z = center_z + ((float)beam - 1.0f) * 1.0f;

		beams[beam] = wreck_model_add_log(&model, make_vector(center_x - 0.2f, beam_y, z), along_x, LOG_RADIUS, 1.4f, WRECK_MATERIAL_WOOD, 0xA87848);
		wreck_model_add_link(&model, beams[beam], below, make_vector(wall_x, wall_top, z), WRECK_LINK_NAILS);
		wreck_model_add_link(&model, beams[beam], lintel, make_vector(lintel_x, wall_top, z), WRECK_LINK_NAILS);
	}
	for (slab = 0; slab < 4; slab++)
	{
		float x = center_x - 1.25f + (float)slab * 0.7f;
		float y = beam_y + LOG_FLAT_TO_FLAT * 0.5f + 0.04f;
		int slab_box = wreck_model_add_box(&model, make_vector(x, y, center_z), b3Quat_identity, slab_half_extents, WRECK_MATERIAL_WOOD, 0xB89868);

		for (beam = 0; beam < 3; beam++)
		{
			wreck_model_add_link(&model, slab_box, beams[beam], make_vector(x, y - 0.04f, center_z + ((float)beam - 1.0f) * 1.0f), WRECK_LINK_NAILS);
		}
	}
}

// A beam on three pillars two meters apart, a heavy crate on its end over
// the first. Without that pillar the crate tips the beam on the middle
// one: the last pillar's link is pulled up.
static void build_beam_on_pillars(float center_x, float center_z)
{
	b3Vec3 pillar_half_extents = make_vector(0.15f, 0.75f, 0.15f);
	b3Vec3 beam_half_extents = make_vector(2.5f, 0.10f, 0.15f);
	b3Vec3 crate_half_extents = make_vector(0.33f, 0.33f, 0.33f);
	int beam = wreck_model_add_box(&model, make_vector(center_x, 1.60f, center_z), b3Quat_identity, beam_half_extents, WRECK_MATERIAL_WOOD, 0xA87848);
	int crate = wreck_model_add_box(&model, make_vector(center_x - 2.1f, 2.03f, center_z), b3Quat_identity, crate_half_extents, WRECK_MATERIAL_BRICK, 0x707888);
	int pillar;

	wreck_model_add_link(&model, crate, beam, make_vector(center_x - 2.1f, 1.70f, center_z), WRECK_LINK_NAILS);
	for (pillar = -1; pillar <= 1; pillar++)
	{
		float x = center_x + (float)pillar * 2.0f;
		int pillar_box = wreck_model_add_box(&model, make_vector(x, 0.75f, center_z), b3Quat_identity, pillar_half_extents, WRECK_MATERIAL_BRICK, 0xB0A898);

		wreck_model_add_link(&model, pillar_box, WRECK_GROUND, make_vector(x, 0.0f, center_z), WRECK_LINK_FOUNDATION);
		wreck_model_add_link(&model, beam, pillar_box, make_vector(x, 1.50f, center_z), WRECK_LINK_NAILS);
	}
}

// ---------------------------------------------------------------------
// The bend scene's three cases

// A plank or a log on two brick pillars 3.6 m apart, with a crate on its
// middle (none when crate_half_size is 0) and, if asked for, a third
// pillar propping the middle.
//
// (A piece is rigid when the support pass works out who bears what, so
// a plank on three pillars in a row leans on all three alike wherever
// the load on it is: the prop takes a third of the crate, not most of
// it, and the plank is reckoned more bent over it than a real one is.)
static void build_span(float center_x, float center_z, bool is_log, float crate_half_size, bool with_prop)
{
	b3Vec3 pillar_half_extents = make_vector(0.15f, 0.75f, 0.15f);
	b3Vec3 plank_half_extents = make_vector(2.0f, 0.025f, 0.15f);
	float span_half_height = is_log ? LOG_FLAT_TO_FLAT * 0.5f : plank_half_extents.y;
	float span_y = 1.5f + span_half_height;
	int span;
	int pillar;

	if (is_log)
	{
		span = wreck_model_add_log(&model, make_vector(center_x, span_y, center_z), b3Quat_identity, LOG_RADIUS, 2.0f, WRECK_MATERIAL_WOOD, 0x9C6B3C);
	}
	else
	{
		span = wreck_model_add_box(&model, make_vector(center_x, span_y, center_z), b3Quat_identity, plank_half_extents, WRECK_MATERIAL_WOOD, 0xB89868);
	}
	for (pillar = -1; pillar <= 1; pillar++)
	{
		float x = center_x + (float)pillar * 1.8f;
		int pillar_box;

		if (pillar == 0 && !with_prop)
		{
			continue;
		}
		pillar_box = wreck_model_add_box(&model, make_vector(x, 0.75f, center_z), b3Quat_identity, pillar_half_extents, WRECK_MATERIAL_BRICK,
			pillar == 0 ? 0x8890A0 : 0xB0A898);
		wreck_model_add_link(&model, pillar_box, WRECK_GROUND, make_vector(x, 0.0f, center_z), WRECK_LINK_FOUNDATION);
		wreck_model_add_link(&model, span, pillar_box, make_vector(x, 1.5f, center_z), WRECK_LINK_NAILS);
	}
	if (crate_half_size > 0.0f)
	{
		float crate_y = span_y + span_half_height + crate_half_size;
		int crate = wreck_model_add_box(&model, make_vector(center_x, crate_y, center_z), b3Quat_identity,
			make_vector(crate_half_size, crate_half_size, crate_half_size), WRECK_MATERIAL_BRICK, 0x707888);

		wreck_model_add_link(&model, crate, span, make_vector(center_x, crate_y - crate_half_size, center_z), WRECK_LINK_NAILS);
	}
}

static void build_bend_structures(void)
{
	// A plank with a crate too heavy for it, held up by a prop under its
	// middle: knock the prop out and the plank breaks under the crate
	build_span(-5.0f, 0.0f, false, 0.28f, true);
	// The same plank with a crate it bears: break it with a ball
	build_span(0.0f, 0.0f, false, 0.20f, false);
	// A log: a ball knocks it off its pillars whole, a fast one breaks it
	build_span(5.0f, 0.0f, true, 0.0f, false);
}

// ---------------------------------------------------------------------
// The house scene

#define HOUSE_HALF_X 2.0f
#define HOUSE_HALF_Z 1.5f
#define HOUSE_POST_COUNT 5
#define HOUSE_WALL_ROWS 11
#define HOUSE_ROOF_ROWS 11
// Half the side of the frame's square timbers; half the thickness and
// half the width of the boards
#define HOUSE_TIMBER 0.05f
#define HOUSE_BOARD_THICKNESS 0.0125f
#define HOUSE_BOARD_WIDTH 0.1f
#define HOUSE_POST_HALF_HEIGHT 1.1f
#define HOUSE_EAVE_OVERHANG 0.25f
#define HOUSE_ROOF_RISE 1.5f

// A timber frame house, all nailed: sills on the foundation, posts,
// plates on the posts, boards across the posts outside (with a door in
// the front wall and a window in the back one), and on the plates a
// roof of five trusses (a tie beam, two rafters, a collar beside them)
// with boards across the rafters. The gables are open.
static void build_house(float center_x, float center_z)
{
	const float timber = HOUSE_TIMBER;
	const float sill_top = 2.0f * timber;
	const float post_y = sill_top + HOUSE_POST_HALF_HEIGHT;
	const float post_top = sill_top + 2.0f * HOUSE_POST_HALF_HEIGHT;
	const float plate_y = post_top + timber;
	const float tie_y = post_top + 3.0f * timber;
	const float tie_top = post_top + 4.0f * timber;
	const float eave_run = HOUSE_HALF_Z + HOUSE_EAVE_OVERHANG;
	const float slope_length = sqrtf(eave_run * eave_run + HOUSE_ROOF_RISE * HOUSE_ROOF_RISE);
	const float slope_angle = atan2f(HOUSE_ROOF_RISE, eave_run);
	const float slope_sine = sinf(slope_angle);
	const float slope_cosine = cosf(slope_angle);
	// (The rafters' axes meet here, over the middle of the house)
	const float ridge_y = tie_top + 0.07f + HOUSE_ROOF_RISE;
	const float rafter_start = 0.08f;
	const uint32_t frame_color = 0x9C6B3C;
	int sills_x[2];
	int sills_z[2];
	int plates_x[2];
	int plates_z[2];
	int posts[2][HOUSE_POST_COUNT];
	int side_posts[2];
	int rafters[2][HOUSE_POST_COUNT];
	int side;
	int post;
	int row;

	// The sills and the plates: those along x run the whole length, those
	// along z fit between them
	for (side = 0; side < 2; side++)
	{
		float sign = side == 0 ? 1.0f : -1.0f;
		float z = center_z + sign * HOUSE_HALF_Z;
		float x = center_x + sign * HOUSE_HALF_X;
		int end;

		sills_x[side] = wreck_model_add_box(&model, make_vector(center_x, timber, z), b3Quat_identity,
			make_vector(HOUSE_HALF_X + timber, timber, timber), WRECK_MATERIAL_WOOD, frame_color);
		plates_x[side] = wreck_model_add_box(&model, make_vector(center_x, plate_y, z), b3Quat_identity,
			make_vector(HOUSE_HALF_X + timber, timber, timber), WRECK_MATERIAL_WOOD, frame_color);
		sills_z[side] = wreck_model_add_box(&model, make_vector(x, timber, center_z), b3Quat_identity,
			make_vector(timber, timber, HOUSE_HALF_Z - timber), WRECK_MATERIAL_WOOD, frame_color);
		plates_z[side] = wreck_model_add_box(&model, make_vector(x, plate_y, center_z), b3Quat_identity,
			make_vector(timber, timber, HOUSE_HALF_Z - timber), WRECK_MATERIAL_WOOD, frame_color);
		// (A sill lies on the foundation all along: held under every
		// post and at its ends)
		for (end = 0; end < HOUSE_POST_COUNT; end++)
		{
			wreck_model_add_link(&model, sills_x[side], WRECK_GROUND,
				make_vector(center_x - HOUSE_HALF_X + (float)end * (2.0f * HOUSE_HALF_X / (float)(HOUSE_POST_COUNT - 1)), 0.0f, z), WRECK_LINK_FOUNDATION);
		}
		for (end = -1; end <= 1; end++)
		{
			wreck_model_add_link(&model, sills_z[side], WRECK_GROUND, make_vector(x, 0.0f, center_z + (float)end * 1.2f), WRECK_LINK_FOUNDATION);
		}
	}
	for (side = 0; side < 2; side++)
	{
		float x = center_x + (side == 0 ? HOUSE_HALF_X : -HOUSE_HALF_X);
		int wall;

		for (wall = 0; wall < 2; wall++)
		{
			float z = center_z + (wall == 0 ? 1.0f : -1.0f) * (HOUSE_HALF_Z - timber);

			wreck_model_add_link(&model, sills_z[side], sills_x[wall], make_vector(x, timber, z), WRECK_LINK_NAILS);
			wreck_model_add_link(&model, plates_z[side], plates_x[wall], make_vector(x, plate_y, z), WRECK_LINK_NAILS);
		}
	}

	// The posts: five in each long wall, one in the middle of each short
	// one
	for (side = 0; side < 2; side++)
	{
		float sign = side == 0 ? 1.0f : -1.0f;
		float z = center_z + sign * HOUSE_HALF_Z;
		float x = center_x + sign * HOUSE_HALF_X;

		for (post = 0; post < HOUSE_POST_COUNT; post++)
		{
			float post_x = center_x - HOUSE_HALF_X + (float)post * (2.0f * HOUSE_HALF_X / (float)(HOUSE_POST_COUNT - 1));

			posts[side][post] = wreck_model_add_box(&model, make_vector(post_x, post_y, z), b3Quat_identity,
				make_vector(timber, HOUSE_POST_HALF_HEIGHT, timber), WRECK_MATERIAL_WOOD, frame_color);
			wreck_model_add_link(&model, posts[side][post], sills_x[side], make_vector(post_x, sill_top, z), WRECK_LINK_NAILS);
			wreck_model_add_link(&model, plates_x[side], posts[side][post], make_vector(post_x, post_top, z), WRECK_LINK_NAILS);
		}
		side_posts[side] = wreck_model_add_box(&model, make_vector(x, post_y, center_z), b3Quat_identity,
			make_vector(timber, HOUSE_POST_HALF_HEIGHT, timber), WRECK_MATERIAL_WOOD, frame_color);
		wreck_model_add_link(&model, side_posts[side], sills_z[side], make_vector(x, sill_top, center_z), WRECK_LINK_NAILS);
		wreck_model_add_link(&model, plates_z[side], side_posts[side], make_vector(x, post_top, center_z), WRECK_LINK_NAILS);
	}

	// The boards of the walls, each nailed to every post it crosses. In
	// the front wall (+z) the door is between the third and fourth posts,
	// all but the top row; in the back wall the window is between the
	// second and third posts, four rows
	for (row = 0; row < HOUSE_WALL_ROWS; row++)
	{
		float y = sill_top + HOUSE_BOARD_WIDTH + (float)row * 2.0f * HOUSE_BOARD_WIDTH;
		uint32_t color = (row & 1) ? 0xBC9C64 : 0xC8A870;

		for (side = 0; side < 2; side++)
		{
			float sign = side == 0 ? 1.0f : -1.0f;
			float board_z = center_z + sign * (HOUSE_HALF_Z + timber + HOUSE_BOARD_THICKNESS);
			float nail_z = center_z + sign * (HOUSE_HALF_Z + timber);
			float board_x = center_x + sign * (HOUSE_HALF_X + timber + HOUSE_BOARD_THICKNESS);
			float nail_x = center_x + sign * (HOUSE_HALF_X + timber);
			// (The posts where an opening starts and ends; none: -1)
			int opening_start = -1;
			int first_post = 0;
			int board;
			int end;

			if (side == 0 && row < HOUSE_WALL_ROWS - 1)
			{
				opening_start = 2;
			}
			if (side == 1 && row >= 5 && row <= 8)
			{
				opening_start = 1;
			}
			while (first_post < HOUSE_POST_COUNT)
			{
				int last_post = opening_start >= first_post ? opening_start : HOUSE_POST_COUNT - 1;
				float start_x = -HOUSE_HALF_X + (float)first_post - timber;
				float end_x = -HOUSE_HALF_X + (float)last_post + timber;

				board = wreck_model_add_box(&model, make_vector(center_x + (start_x + end_x) * 0.5f, y, board_z), b3Quat_identity,
					make_vector((end_x - start_x) * 0.5f, HOUSE_BOARD_WIDTH, HOUSE_BOARD_THICKNESS), WRECK_MATERIAL_WOOD, color);
				for (post = first_post; post <= last_post; post++)
				{
					wreck_model_add_link(&model, board, posts[side][post], make_vector(center_x - HOUSE_HALF_X + (float)post, y, nail_z), WRECK_LINK_NAILS);
				}
				first_post = last_post + 1;
			}

			// The short wall at this side's end of x
			board = wreck_model_add_box(&model, make_vector(board_x, y, center_z), b3Quat_identity,
				make_vector(HOUSE_BOARD_THICKNESS, HOUSE_BOARD_WIDTH, HOUSE_HALF_Z + timber), WRECK_MATERIAL_WOOD, color);
			for (end = 0; end < 2; end++)
			{
				wreck_model_add_link(&model, board, posts[end][side == 0 ? HOUSE_POST_COUNT - 1 : 0],
					make_vector(nail_x, y, center_z + (end == 0 ? HOUSE_HALF_Z : -HOUSE_HALF_Z)), WRECK_LINK_NAILS);
			}
			wreck_model_add_link(&model, board, side_posts[side], make_vector(nail_x, y, center_z), WRECK_LINK_NAILS);
		}
	}

	// The trusses, one over each pair of posts: a tie beam across the
	// plates, two rafters from its ends up to the ridge, and a collar
	// beside the rafters half way up
	for (post = 0; post < HOUSE_POST_COUNT; post++)
	{
		float x = center_x - HOUSE_HALF_X + (float)post;
		float collar_x = x + (post == HOUSE_POST_COUNT - 1 ? -2.0f : 2.0f) * timber;
		float half_way = slope_length * 0.5f;
		float collar_y = ridge_y - slope_sine * half_way;
		float collar_reach = slope_cosine * half_way;
		int tie = wreck_model_add_box(&model, make_vector(x, tie_y, center_z), b3Quat_identity, make_vector(timber, timber, eave_run), WRECK_MATERIAL_WOOD, frame_color);
		int collar = wreck_model_add_box(&model, make_vector(collar_x, collar_y, center_z), b3Quat_identity,
			make_vector(timber, timber, collar_reach + 2.0f * timber), WRECK_MATERIAL_WOOD, 0x8A5A30);

		for (side = 0; side < 2; side++)
		{
			float sign = side == 0 ? 1.0f : -1.0f;
			float middle = (rafter_start + slope_length) * 0.5f;

			wreck_model_add_link(&model, tie, plates_x[side], make_vector(x, post_top + 2.0f * timber, center_z + sign * HOUSE_HALF_Z), WRECK_LINK_NAILS);
			rafters[side][post] = wreck_model_add_box(&model,
				make_vector(x, ridge_y - slope_sine * middle, center_z + sign * slope_cosine * middle),
				b3MakeQuatFromAxisAngle(make_vector(1.0f, 0.0f, 0.0f), sign * slope_angle),
				make_vector(timber, timber, (slope_length - rafter_start) * 0.5f), WRECK_MATERIAL_WOOD, frame_color);
			wreck_model_add_link(&model, rafters[side][post], tie, make_vector(x, tie_top, center_z + sign * (eave_run - timber)), WRECK_LINK_NAILS);
			wreck_model_add_link(&model, collar, rafters[side][post],
				make_vector((x + collar_x) * 0.5f, collar_y, center_z + sign * collar_reach), WRECK_LINK_NAILS);
		}
		wreck_model_add_link(&model, rafters[0][post], rafters[1][post], make_vector(x, ridge_y, center_z), WRECK_LINK_NAILS);
	}

	// The boards of the roof, across the rafters, nailed to each
	for (side = 0; side < 2; side++)
	{
		float sign = side == 0 ? 1.0f : -1.0f;
		b3Quat rotation = b3MakeQuatFromAxisAngle(make_vector(1.0f, 0.0f, 0.0f), sign * slope_angle);

		for (row = 0; row < HOUSE_ROOF_ROWS; row++)
		{
			float along = 0.2f + (float)row * 2.0f * HOUSE_BOARD_WIDTH;
			float on_rafter_y = ridge_y - slope_sine * along + slope_cosine * timber;
			float on_rafter_z = center_z + sign * (slope_cosine * along + slope_sine * timber);
			int board = wreck_model_add_box(&model,
				make_vector(center_x, on_rafter_y + slope_cosine * HOUSE_BOARD_THICKNESS, on_rafter_z + sign * slope_sine * HOUSE_BOARD_THICKNESS), rotation,
				make_vector(HOUSE_HALF_X + HOUSE_EAVE_OVERHANG, HOUSE_BOARD_THICKNESS, HOUSE_BOARD_WIDTH), WRECK_MATERIAL_WOOD, (row & 1) ? 0x7A4E2C : 0x8A5A30);

			for (post = 0; post < HOUSE_POST_COUNT; post++)
			{
				wreck_model_add_link(&model, board, rafters[side][post], make_vector(center_x - HOUSE_HALF_X + (float)post, on_rafter_y, on_rafter_z), WRECK_LINK_NAILS);
			}
		}
	}
}

static void build_house_structures(void)
{
	build_house(0.0f, 0.0f);
}

static void build_cabin_structures(void)
{
	build_cabin(-3.0f, 0.0f);
	build_wall(3.0f, 1.0f);
}

static void build_support_structures(void)
{
	build_wall_on_pane(-6.0f, 0.0f);
	build_roof_on_wall(0.0f, 0.0f);
	build_beam_on_pillars(6.5f, 0.0f);
}

static void build_world(void)
{
	b3WorldDef definition = b3DefaultWorldDef();

	memset(&cabin, 0, sizeof(cabin));
	// The numbers are read at every start: change the ini, start again
	wreck_settings_default(&cabin.settings);
	if (!damage_ini_read(SETTINGS_PATH, wreck_settings_set, &cabin.settings) && !settings_missing_was_said)
	{
		fprintf(stderr, "no %s here, the built in numbers are used\n", SETTINGS_PATH);
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
	wreck_model_create(&model, cabin.world, &cabin.settings);
	build_structures();
	wreck_model_finish(&model);
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
			wreck_model_add_projectile(&model, body, cabin.settings.ball_radius);
		}
		else
		{
			blast_mark_t *mark = &cabin.recent_blasts[cabin.blast_count % RECENT_BLASTS_MAX];

			wreck_model_blast(&model, position, event->strength, cabin.frame_index + 1);
			mark->frame_index = cabin.frame_index + 1;
			mark->position = position;
			cabin.blast_count++;
		}
	}
}

static void step_world(void)
{
	b3ContactEvents events;
	int i;

	set_off_events();
	wreck_model_before_step(&model);
	b3World_Step(cabin.world, STEP_SECONDS, SUBSTEP_COUNT);
	cabin.frame_index++;
	// The step's hits, each to the model whose piece it is (there is but
	// one here; the engine does the same for many)
	events = b3World_GetContactEvents(cabin.world);
	for (i = 0; i < events.hitCount; i++)
	{
		const b3ContactHitEvent *event = &events.hitEvents[i];

		if (wreck_shape_model(event->shapeIdA) == &model || wreck_shape_model(event->shapeIdB) == &model)
		{
			wreck_model_take_hit(&model, event, cabin.frame_index);
		}
	}
	wreck_model_after_step(&model, cabin.frame_index, STEP_SECONDS);
}

static bool cabin_scene_open(void)
{
	if (build_structures != build_cabin_structures)
	{
		inspected_piece = -1;
	}
	build_structures = build_cabin_structures;
	build_world();
	return true;
}

static bool house_scene_open(void)
{
	if (build_structures != build_house_structures)
	{
		inspected_piece = -1;
	}
	build_structures = build_house_structures;
	build_world();
	return true;
}

static void house_scene_default_camera(soft_render_camera_t *camera)
{
	camera->eye[0] = 5.5f;
	camera->eye[1] = 3.2f;
	camera->eye[2] = 7.5f;
	camera->target[0] = 0.0f;
	camera->target[1] = 1.8f;
	camera->target[2] = 0.0f;
	camera->vertical_field_of_view_degrees = 60.0f;
	camera->near_distance = 0.05f;
}

static bool bend_scene_open(void)
{
	if (build_structures != build_bend_structures)
	{
		inspected_piece = -1;
	}
	build_structures = build_bend_structures;
	build_world();
	return true;
}

static void bend_scene_default_camera(soft_render_camera_t *camera)
{
	camera->eye[0] = 0.0f;
	camera->eye[1] = 2.8f;
	camera->eye[2] = 8.5f;
	camera->target[0] = 0.0f;
	camera->target[1] = 1.2f;
	camera->target[2] = 0.0f;
	camera->vertical_field_of_view_degrees = 60.0f;
	camera->near_distance = 0.05f;
}

static bool support_scene_open(void)
{
	if (build_structures != build_support_structures)
	{
		inspected_piece = -1;
	}
	build_structures = build_support_structures;
	build_world();
	return true;
}

static void support_scene_default_camera(soft_render_camera_t *camera)
{
	camera->eye[0] = 0.5f;
	camera->eye[1] = 2.6f;
	camera->eye[2] = 9.5f;
	camera->target[0] = 0.5f;
	camera->target[1] = 1.0f;
	camera->target[2] = 0.0f;
	camera->vertical_field_of_view_degrees = 60.0f;
	camera->near_distance = 0.05f;
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
	// The pieces first, their backs not drawn (they are closed), then
	// the ground, of which only what shows between them is filled
	renderer->cull_back_faces = true;
	damage_draw_solids(&model, renderer, 2);
	renderer->cull_back_faces = false;
	soft_render_set_object(renderer, 1);
	soft_render_ground(renderer, 0.0f, 0.0f, 0.0f, 40.0f, 1.0f, GROUND_COLOR_FIRST, GROUND_COLOR_SECOND);
	if (draw_outlines)
	{
		soft_render_outlines(renderer);
	}
	damage_draw_marks(&model, renderer, cabin.frame_index, draw_debug);
	if (inspected_piece >= 0)
	{
		damage_draw_inspection(&model, renderer, inspected_piece, 6, 74, 2);
	}

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
	wreck_model_describe(&model, text, text_size);
}

static uint32_t cabin_scene_checksum(void)
{
	return wreck_model_checksum(&model);
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

static void cabin_scene_inspect(const float origin[3], const float direction[3], float distance)
{
	b3RayResult result = b3World_CastRayClosest(cabin.world, make_vector(origin[0], origin[1], origin[2]),
		make_vector(direction[0] * distance, direction[1] * distance, direction[2] * distance), b3DefaultQueryFilter());

	inspected_piece = result.hit ? wreck_model_shape_piece(&model, result.shapeId) : -1;
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
	.inspect = cabin_scene_inspect,
};

const damage_scene_t house_scene =
{
	.name = "house",
	.about = "a nailed timber frame house: posts, boards, a roof of trusses with boards across",
	.open = house_scene_open,
	.close = cabin_scene_close,
	.default_camera = house_scene_default_camera,
	.seek = cabin_scene_seek,
	.draw = cabin_scene_draw,
	.describe = cabin_scene_describe,
	.checksum = cabin_scene_checksum,
	.cast_ray = cabin_scene_cast_ray,
	.inspect = cabin_scene_inspect,
};

const damage_scene_t bend_scene =
{
	.name = "bend",
	.about = "long pieces that bend and break: a plank under a crate, a plank and a log to throw balls at",
	.open = bend_scene_open,
	.close = cabin_scene_close,
	.default_camera = bend_scene_default_camera,
	.seek = cabin_scene_seek,
	.draw = cabin_scene_draw,
	.describe = cabin_scene_describe,
	.checksum = cabin_scene_checksum,
	.cast_ray = cabin_scene_cast_ray,
	.inspect = cabin_scene_inspect,
};

const damage_scene_t support_scene =
{
	.name = "support",
	.about = "what holds what up: a wall on a pane, a roof on one wall, a beam on three pillars",
	.open = support_scene_open,
	.close = cabin_scene_close,
	.default_camera = support_scene_default_camera,
	.seek = cabin_scene_seek,
	.draw = cabin_scene_draw,
	.describe = cabin_scene_describe,
	.checksum = cabin_scene_checksum,
	.cast_ray = cabin_scene_cast_ray,
	.inspect = cabin_scene_inspect,
};
