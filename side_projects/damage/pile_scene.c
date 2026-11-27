// The pile: see pile_scene.h

#include "pile_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "box3d/box3d.h"

#include "damage_events.h"

#define STEP_SECONDS (1.0f / (float)DAMAGE_SCENE_FRAMES_PER_SECOND)
#define SUBSTEP_COUNT 4
#define GRAVITY 9.81f
// Hits slower than this are not reported (meters a second)
#define HIT_SPEED_THRESHOLD 1.0f

#define PIECES_MAX 256
#define RECENT_HITS_MAX 64
#define RECENT_BLASTS_MAX 16
#define HIT_SHOWN_FRAMES 20
#define BLAST_SHOWN_FRAMES 12
#define BODY_CONTACTS_MAX 64

// Kilograms a cubic meter
#define WOOD_DENSITY 600.0f
#define BRICK_DENSITY 1800.0f
#define STEEL_DENSITY 7800.0f
#define FRICTION 0.6f

// A log is a prism of this many flat sides (Box3D has no cylinder: its
// shapes are spheres, capsules and convex hulls), turned so that it lies
// on a flat side, not on an edge
#define LOG_SIDE_COUNT 12
#define LOG_RADIUS 0.15f
#define PI 3.14159265358979f
#define LOG_FIRST_CORNER_RADIANS (PI / (float)LOG_SIDE_COUNT)

// What happens when in the pile's film (frames from the start)
#define RAFT_SPLIT_FRAME 40
#define BALL_FRAME 150
#define BLAST_FRAME 330
// A thrown ball (the film's and the events'): steel, this big
#define BALL_RADIUS 0.3f
// How far a blast reaches at full strength, and how far beyond it fades
#define BLAST_RADIUS 1.5f
#define BLAST_FALLOFF 3.0f
#define FILM_BLAST_STRENGTH 600.0f

#define SKY_COLOR 0x9DB7D1
#define GROUND_COLOR_FIRST 0x6E7B5A
#define GROUND_COLOR_SECOND 0x626E50
#define ASLEEP_COLOR 0x707070
#define HIT_COLOR 0xFF2020
#define CONTACT_COLOR 0xFFE040
#define IMPULSE_COLOR 0xFF9020
#define LABEL_COLOR 0xFFFFFF
// A contact's impulse is drawn as a line this long a newton second
#define IMPULSE_METERS_PER_NEWTON_SECOND 0.002f

typedef enum
{
	PIECE_BOX,
	PIECE_LOG,
	PIECE_BALL
} piece_kind_t;

// One solid thing. Several pieces may share a body (the raft): each then
// has its own place in it.
typedef struct
{
	piece_kind_t kind;
	b3BodyId body;
	b3Vec3 local_position;
	b3Quat local_rotation;
	b3Vec3 half_extents;               // a box's
	float radius;                      // a log's or a ball's
	float half_length;                 // a log's, along its own x
	uint32_t color;
} piece_t;

typedef struct
{
	int frame_index;                   // the frame it shows in first
	b3Vec3 point;
	float approach_speed;
} hit_t;

typedef struct
{
	int frame_index;                   // the frame it shows in first
	b3Vec3 position;
} blast_mark_t;

typedef struct
{
	bool is_open;
	b3WorldId world;
	piece_t pieces[PIECES_MAX];
	int piece_count;
	int frame_index;
	int raft_first_piece;
	int raft_piece_count;
	hit_t recent_hits[RECENT_HITS_MAX];
	int hit_count;                     // all there were; the last RECENT_HITS_MAX are kept
	float fastest_hit_speed;
	blast_mark_t recent_blasts[RECENT_BLASTS_MAX];
	int blast_count;                   // all there were; the last RECENT_BLASTS_MAX are kept
} pile_t;

static pile_t pile;
// The pile scene has a film (logs falling, the raft, a ball, a blast, all
// at set frames); the sandbox is the wall and the crib left alone, for
// whatever the person at the keys throws at them
static bool world_has_film;

static b3Vec3 make_vector(float x, float y, float z)
{
	b3Vec3 vector = {x, y, z};

	return vector;
}

// Box3D's vector and quaternion as the plain floats the soft renderer
// takes (x, y, z and x, y, z, w: the same layout)
static const float *vector_floats(const b3Vec3 *vector)
{
	return (const float *)vector;
}

static const float *rotation_floats(const b3Quat *rotation)
{
	return (const float *)rotation;
}

static b3Quat rotation_about(float axis_x, float axis_y, float axis_z, float degrees)
{
	b3Vec3 axis = make_vector(axis_x, axis_y, axis_z);
	float length = b3Length(axis);

	return b3MakeQuatFromAxisAngle(b3MulSV(1.0f / length, axis), degrees * PI / 180.0f);
}

static int add_piece(piece_kind_t kind, b3BodyId body, b3Vec3 local_position, b3Quat local_rotation, uint32_t color)
{
	piece_t *piece;

	if (pile.piece_count >= PIECES_MAX)
	{
		return -1;
	}
	piece = &pile.pieces[pile.piece_count];
	memset(piece, 0, sizeof(*piece));
	piece->kind = kind;
	piece->body = body;
	piece->local_position = local_position;
	piece->local_rotation = local_rotation;
	piece->color = color;
	return pile.piece_count++;
}

static b3BodyId create_body(b3BodyType type, b3Vec3 position, b3Quat rotation)
{
	b3BodyDef definition = b3DefaultBodyDef();

	definition.type = type;
	definition.position = position;
	definition.rotation = rotation;
	return b3CreateBody(pile.world, &definition);
}

// A shape's user data is its piece's number plus one (0: the ground)
static b3ShapeDef shape_definition(float density, int piece_index)
{
	b3ShapeDef definition = b3DefaultShapeDef();

	definition.density = density;
	definition.baseMaterial.friction = FRICTION;
	definition.enableHitEvents = true;
	definition.userData = (void *)(intptr_t)(piece_index + 1);
	return definition;
}

// A log's collision shape: a prism along x. The world keeps its own copy
// of a hull given to a shape, so the caller destroys this one.
static b3HullData *create_log_hull(float half_length)
{
	b3Vec3 points[LOG_SIDE_COUNT * 2];
	int i;

	for (i = 0; i < LOG_SIDE_COUNT; i++)
	{
		float angle = LOG_FIRST_CORNER_RADIANS + 2.0f * PI * (float)i / (float)LOG_SIDE_COUNT;
		float y = LOG_RADIUS * cosf(angle);
		float z = LOG_RADIUS * sinf(angle);

		points[i * 2] = make_vector(-half_length, y, z);
		points[i * 2 + 1] = make_vector(half_length, y, z);
	}
	return b3CreateHull(points, LOG_SIDE_COUNT * 2, LOG_SIDE_COUNT * 2);
}

// A log as one more shape of a body, at its own place in it
static void attach_log(b3BodyId body, b3Vec3 local_position, b3Quat local_rotation, float half_length, uint32_t color)
{
	int piece_index = add_piece(PIECE_LOG, body, local_position, local_rotation, color);
	b3ShapeDef definition = shape_definition(WOOD_DENSITY, piece_index);
	b3HullData *hull;
	b3Transform local_transform;

	if (piece_index < 0)
	{
		return;
	}
	pile.pieces[piece_index].radius = LOG_RADIUS;
	pile.pieces[piece_index].half_length = half_length;
	hull = create_log_hull(half_length);
	if (hull == NULL)
	{
		return;
	}
	local_transform.p = local_position;
	local_transform.q = local_rotation;
	b3CreateTransformedHullShape(body, &definition, hull, local_transform, b3Vec3_one);
	b3DestroyHull(hull);
}

static b3BodyId add_log(b3Vec3 position, b3Quat rotation, float half_length, uint32_t color)
{
	b3BodyId body = create_body(b3_dynamicBody, position, rotation);

	attach_log(body, b3Vec3_zero, b3Quat_identity, half_length, color);
	return body;
}

static b3BodyId add_box(b3Vec3 position, b3Quat rotation, b3Vec3 half_extents, float density, uint32_t color)
{
	b3BodyId body = create_body(b3_dynamicBody, position, rotation);
	int piece_index = add_piece(PIECE_BOX, body, b3Vec3_zero, b3Quat_identity, color);
	b3ShapeDef definition = shape_definition(density, piece_index);
	b3BoxHull hull = b3MakeBoxHull(half_extents.x, half_extents.y, half_extents.z);

	if (piece_index >= 0)
	{
		pile.pieces[piece_index].half_extents = half_extents;
		b3CreateHullShape(body, &definition, &hull.base);
	}
	return body;
}

static b3BodyId add_ball(b3Vec3 position, float radius, float density, uint32_t color)
{
	b3BodyId body = create_body(b3_dynamicBody, position, b3Quat_identity);
	int piece_index = add_piece(PIECE_BALL, body, b3Vec3_zero, b3Quat_identity, color);
	b3ShapeDef definition = shape_definition(density, piece_index);
	b3Sphere sphere;

	sphere.center = b3Vec3_zero;
	sphere.radius = radius;
	if (piece_index >= 0)
	{
		pile.pieces[piece_index].radius = radius;
		b3CreateSphereShape(body, &definition, &sphere);
	}
	return body;
}

static void build_ground(void)
{
	b3BodyId body = create_body(b3_staticBody, make_vector(0.0f, -0.5f, 0.0f), b3Quat_identity);
	b3ShapeDef definition = shape_definition(0.0f, -1);
	b3BoxHull hull = b3MakeBoxHull(40.0f, 0.5f, 40.0f);

	b3CreateHullShape(body, &definition, &hull.base);
}

// Bricks laid dry, each row half a brick along from the one under it
static void build_wall(float center_x, float center_z)
{
	b3Vec3 half_extents = make_vector(0.30f, 0.15f, 0.15f);
	int row;
	int brick;

	for (row = 0; row < 8; row++)
	{
		// (An odd row has a brick less: none of them hangs over the end)
		for (brick = 0; brick < ((row & 1) ? 5 : 6); brick++)
		{
			float x = center_x + ((float)brick - 2.5f) * 0.60f + ((row & 1) ? 0.30f : 0.0f);
			float y = 0.15f + (float)row * 0.30f;

			add_box(make_vector(x, y, center_z), b3Quat_identity, half_extents, BRICK_DENSITY, ((row + brick) & 1) ? 0xA8503A : 0x96452F);
		}
	}
}

// Logs stacked two by two, each pair across the pair under it, lying on
// one another by their own weight alone (no notches, nothing holds them)
static void build_crib(float center_x, float center_z)
{
	float flat_to_flat = 2.0f * LOG_RADIUS * cosf(LOG_FIRST_CORNER_RADIANS);
	int course;
	int side;

	for (course = 0; course < 6; course++)
	{
		for (side = -1; side <= 1; side += 2)
		{
			float y = flat_to_flat * 0.5f + (float)course * (flat_to_flat + 0.002f);
			uint32_t color = (course & 1) ? 0x8A5A30 : 0x9C6B3C;

			if (course & 1)
			{
				add_log(make_vector(center_x + (float)side * 1.2f, y, center_z), rotation_about(0.0f, 1.0f, 0.0f, 90.0f), 1.6f, color);
			}
			else
			{
				add_log(make_vector(center_x, y, center_z + (float)side * 1.2f), b3Quat_identity, 1.6f, color);
			}
		}
	}
}

// Logs let go above the crib, each turned its own way
static void build_falling_logs(float center_x, float center_z)
{
	add_log(make_vector(center_x + 0.2f, 4.0f, center_z), rotation_about(0.0f, 1.0f, 0.2f, 35.0f), 1.3f, 0xA87848);
	add_log(make_vector(center_x - 0.3f, 5.2f, center_z + 0.3f), rotation_about(0.3f, 1.0f, 0.0f, 110.0f), 1.3f, 0xB08050);
	add_log(make_vector(center_x, 6.4f, center_z - 0.4f), rotation_about(0.0f, 0.2f, 1.0f, 70.0f), 1.0f, 0xA87848);
	add_log(make_vector(center_x + 0.5f, 7.6f, center_z + 0.2f), rotation_about(1.0f, 1.0f, 0.0f, 20.0f), 1.6f, 0xB08050);
}

// Four logs as one body: two side by side, two across on top of them,
// thrown up turning
static void build_raft(float center_x, float center_z)
{
	float flat_to_flat = 2.0f * LOG_RADIUS * cosf(LOG_FIRST_CORNER_RADIANS);
	b3BodyId body = create_body(b3_dynamicBody, make_vector(center_x, 3.0f, center_z), rotation_about(1.0f, 0.0f, 0.3f, 15.0f));
	b3Quat quarter_turn = rotation_about(0.0f, 1.0f, 0.0f, 90.0f);

	pile.raft_first_piece = pile.piece_count;
	attach_log(body, make_vector(0.0f, 0.0f, -0.5f), b3Quat_identity, 1.0f, 0xC09860);
	attach_log(body, make_vector(0.0f, 0.0f, 0.5f), b3Quat_identity, 1.0f, 0xB48C54);
	attach_log(body, make_vector(-0.5f, flat_to_flat, 0.0f), quarter_turn, 1.0f, 0xC09860);
	attach_log(body, make_vector(0.5f, flat_to_flat, 0.0f), quarter_turn, 1.0f, 0xB48C54);
	pile.raft_piece_count = pile.piece_count - pile.raft_first_piece;
	b3Body_SetLinearVelocity(body, make_vector(0.0f, 7.0f, 0.0f));
	b3Body_SetAngularVelocity(body, make_vector(1.5f, 0.7f, 2.0f));
}

static void build_world(void)
{
	b3WorldDef definition = b3DefaultWorldDef();

	memset(&pile, 0, sizeof(pile));
	definition.gravity = make_vector(0.0f, -GRAVITY, 0.0f);
	// One thread: the same steps must give the same world every time
	definition.workerCount = 1;
	definition.enableSleep = true;
	definition.hitEventThreshold = HIT_SPEED_THRESHOLD;
	pile.world = b3CreateWorld(&definition);
	pile.is_open = true;

	build_ground();
	build_wall(3.0f, 3.0f);
	build_crib(-4.0f, -1.0f);
	if (world_has_film)
	{
		build_falling_logs(-4.0f, -1.0f);
		build_raft(4.5f, -3.0f);
	}
}

// Takes a body of several pieces apart in mid motion: a body for each
// piece, where the piece is and moving as that part of the old body was
// (its speed, and what its turning adds that far from its center).
// What a group breaking in two will come to.
static void split_body(int first_piece, int piece_count)
{
	b3BodyId old_body = pile.pieces[first_piece].body;
	b3WorldTransform old_transform = b3Body_GetTransform(old_body);
	b3Vec3 old_center = b3Body_GetWorldCenter(old_body);
	b3Vec3 linear_velocity = b3Body_GetLinearVelocity(old_body);
	b3Vec3 angular_velocity = b3Body_GetAngularVelocity(old_body);
	int i;

	for (i = first_piece; i < first_piece + piece_count; i++)
	{
		piece_t *piece = &pile.pieces[i];
		b3Vec3 position = b3Add(old_transform.p, b3RotateVector(old_transform.q, piece->local_position));
		b3Quat rotation = b3NormalizeQuat(b3MulQuat(old_transform.q, piece->local_rotation));
		b3BodyId body = create_body(b3_dynamicBody, position, rotation);
		b3ShapeDef definition = shape_definition(WOOD_DENSITY, i);
		b3HullData *hull = create_log_hull(piece->half_length);

		if (hull != NULL)
		{
			b3CreateHullShape(body, &definition, hull);
			b3DestroyHull(hull);
		}
		// (A log's center of mass is its own middle)
		b3Body_SetLinearVelocity(body, b3Add(linear_velocity, b3Cross(angular_velocity, b3Sub(position, old_center))));
		b3Body_SetAngularVelocity(body, angular_velocity);
		piece->body = body;
		piece->local_position = b3Vec3_zero;
		piece->local_rotation = b3Quat_identity;
	}
	b3DestroyBody(old_body);
}

static void throw_ball(b3Vec3 position, b3Vec3 velocity)
{
	b3BodyId body = add_ball(position, BALL_RADIUS, STEEL_DENSITY, 0x505868);

	b3Body_SetLinearVelocity(body, velocity);
}

static void blast(b3Vec3 position, float strength)
{
	b3ExplosionDef definition = b3DefaultExplosionDef();
	blast_mark_t *mark = &pile.recent_blasts[pile.blast_count % RECENT_BLASTS_MAX];

	definition.position = position;
	definition.radius = BLAST_RADIUS;
	definition.falloff = BLAST_FALLOFF;
	definition.impulsePerArea = strength;
	b3World_Explode(pile.world, &definition);
	mark->frame_index = pile.frame_index + 1;
	mark->position = position;
	pile.blast_count++;
}

// The balls and blasts of the person at the keys that belong to this frame
static void set_off_events(void)
{
	int i;

	for (i = 0; i < damage_events_count(); i++)
	{
		const damage_event_t *event = damage_events_get(i);

		if (event->frame_index != pile.frame_index)
		{
			continue;
		}
		if (event->kind == DAMAGE_EVENT_BALL)
		{
			throw_ball(make_vector(event->position[0], event->position[1], event->position[2]),
				make_vector(event->velocity[0], event->velocity[1], event->velocity[2]));
		}
		else
		{
			blast(make_vector(event->position[0], event->position[1], event->position[2]), event->strength);
		}
	}
}

// One frame on: what the frame's number sets off (the film's doings and
// the events), a step of the world, then the hits that step had
static void step_world(void)
{
	b3ContactEvents events;
	int i;

	if (world_has_film)
	{
		if (pile.frame_index == RAFT_SPLIT_FRAME)
		{
			split_body(pile.raft_first_piece, pile.raft_piece_count);
		}
		if (pile.frame_index == BALL_FRAME)
		{
			throw_ball(make_vector(3.0f, 1.3f, 11.0f), make_vector(0.0f, 2.0f, -16.0f));
		}
		if (pile.frame_index == BLAST_FRAME)
		{
			blast(make_vector(-4.0f, 0.4f, -1.0f), FILM_BLAST_STRENGTH);
		}
	}
	set_off_events();
	b3World_Step(pile.world, STEP_SECONDS, SUBSTEP_COUNT);
	pile.frame_index++;

	events = b3World_GetContactEvents(pile.world);
	for (i = 0; i < events.hitCount; i++)
	{
		hit_t *hit = &pile.recent_hits[pile.hit_count % RECENT_HITS_MAX];

		hit->frame_index = pile.frame_index;
		hit->point = events.hitEvents[i].point;
		hit->approach_speed = events.hitEvents[i].approachSpeed;
		if (hit->approach_speed > pile.fastest_hit_speed)
		{
			pile.fastest_hit_speed = hit->approach_speed;
		}
		pile.hit_count++;
	}
}

static bool pile_scene_open(void)
{
	world_has_film = true;
	build_world();
	return true;
}

static bool sandbox_scene_open(void)
{
	world_has_film = false;
	build_world();
	return true;
}

static void pile_scene_close(void)
{
	if (pile.is_open)
	{
		b3DestroyWorld(pile.world);
		pile.is_open = false;
	}
}

static void pile_scene_default_camera(soft_render_camera_t *camera)
{
	camera->eye[0] = 3.5f;
	camera->eye[1] = 4.2f;
	camera->eye[2] = 10.5f;
	camera->target[0] = -0.5f;
	camera->target[1] = 1.2f;
	camera->target[2] = 0.0f;
	camera->vertical_field_of_view_degrees = 60.0f;
	camera->near_distance = 0.05f;
}

static void pile_scene_seek(int frame_index)
{
	if (frame_index < 0)
	{
		frame_index = 0;
	}
	if (frame_index < pile.frame_index)
	{
		pile_scene_close();
		build_world();
	}
	while (pile.frame_index < frame_index)
	{
		step_world();
	}
}

// A body's color when it sleeps: half way to gray
static uint32_t asleep_color(uint32_t color)
{
	uint32_t red = (((color >> 16) & 0xFF) + ((ASLEEP_COLOR >> 16) & 0xFF)) / 2;
	uint32_t green = (((color >> 8) & 0xFF) + ((ASLEEP_COLOR >> 8) & 0xFF)) / 2;
	uint32_t blue = ((color & 0xFF) + (ASLEEP_COLOR & 0xFF)) / 2;

	return (red << 16) | (green << 8) | blue;
}

static void draw_piece(soft_render_t *renderer, const piece_t *piece)
{
	b3WorldTransform body_transform = b3Body_GetTransform(piece->body);
	b3Vec3 position = b3Add(body_transform.p, b3RotateVector(body_transform.q, piece->local_position));
	b3Quat rotation = b3MulQuat(body_transform.q, piece->local_rotation);
	uint32_t color = b3Body_IsAwake(piece->body) ? piece->color : asleep_color(piece->color);

	if (piece->kind == PIECE_BOX)
	{
		soft_render_box(renderer, vector_floats(&position), rotation_floats(&rotation), vector_floats(&piece->half_extents), color);
	}
	else if (piece->kind == PIECE_LOG)
	{
		// The very prism its collision shape is: its ends, and where its
		// first corner points
		b3Vec3 half_axis = b3RotateVector(rotation, make_vector(piece->half_length, 0.0f, 0.0f));
		b3Vec3 end_a = b3Sub(position, half_axis);
		b3Vec3 end_b = b3Add(position, half_axis);
		b3Vec3 first_corner = b3RotateVector(rotation, make_vector(0.0f, cosf(LOG_FIRST_CORNER_RADIANS), sinf(LOG_FIRST_CORNER_RADIANS)));

		soft_render_prism(renderer, vector_floats(&end_a), vector_floats(&end_b), vector_floats(&first_corner), piece->radius, LOG_SIDE_COUNT, color);
	}
	else
	{
		soft_render_sphere(renderer, vector_floats(&position), piece->radius, color);
	}
}

// True when a piece is the first of its body (the pieces of a body follow
// one another)
static bool piece_starts_body(int piece_index)
{
	return piece_index == 0 || !B3_ID_EQUALS(pile.pieces[piece_index].body, pile.pieces[piece_index - 1].body);
}

// Goes over every contact point there is, once each: counts them, finds
// the largest impulse, and draws them if given something to draw on (a
// dot where the two touch, a line along the push as long as its impulse)
static void visit_contacts(soft_render_t *renderer, int *point_count, float *largest_impulse)
{
	b3ContactData contacts[BODY_CONTACTS_MAX];
	int piece_index;
	int contact;
	int manifold_index;
	int point_index;

	*point_count = 0;
	*largest_impulse = 0.0f;
	for (piece_index = 0; piece_index < pile.piece_count; piece_index++)
	{
		b3BodyId body = pile.pieces[piece_index].body;
		int contact_count;

		if (!piece_starts_body(piece_index))
		{
			continue;
		}
		contact_count = b3Body_GetContactData(body, contacts, BODY_CONTACTS_MAX);
		for (contact = 0; contact < contact_count; contact++)
		{
			b3BodyId body_a = b3Shape_GetBody(contacts[contact].shapeIdA);
			bool a_is_ground = b3Shape_GetUserData(contacts[contact].shapeIdA) == NULL;
			b3Vec3 center_a;

			// Two bodies both list the contact between them: it is taken
			// from the first one's side (the ground has no turn here)
			if (!B3_ID_EQUALS(body_a, body) && !a_is_ground)
			{
				continue;
			}
			center_a = b3Body_GetWorldCenter(body_a);
			for (manifold_index = 0; manifold_index < contacts[contact].manifoldCount; manifold_index++)
			{
				const b3Manifold *manifold = &contacts[contact].manifolds[manifold_index];

				for (point_index = 0; point_index < manifold->pointCount; point_index++)
				{
					const b3ManifoldPoint *point = &manifold->points[point_index];
					float impulse = point->totalNormalImpulse;

					(*point_count)++;
					if (impulse > *largest_impulse)
					{
						*largest_impulse = impulse;
					}
					if (renderer != NULL)
					{
						b3Vec3 where = b3Add(center_a, point->anchorA);
						float length = impulse * IMPULSE_METERS_PER_NEWTON_SECOND;
						b3Vec3 line_end;

						if (length > 1.0f)
						{
							length = 1.0f;
						}
						line_end = b3Add(where, b3MulSV(length, manifold->normal));
						soft_render_line(renderer, vector_floats(&where), vector_floats(&line_end), IMPULSE_COLOR);
						soft_render_point(renderer, vector_floats(&where), 3, CONTACT_COLOR);
					}
				}
			}
		}
	}
}

static void draw_label(soft_render_t *renderer, b3Vec3 position, const char *text)
{
	float screen_x;
	float screen_y;

	if (soft_render_project(renderer, vector_floats(&position), &screen_x, &screen_y))
	{
		soft_render_text(renderer, (int)screen_x + 6, (int)screen_y - 4, 1, LABEL_COLOR, text);
	}
}

// What the first thing along a ray is, within a distance. False when
// there is nothing.
static bool pile_scene_cast_ray(const float origin[3], const float direction[3], float distance, float hit_point[3])
{
	b3RayResult result = b3World_CastRayClosest(pile.world, make_vector(origin[0], origin[1], origin[2]),
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

static void pile_scene_draw(soft_render_t *renderer, const soft_render_camera_t *camera, bool draw_outlines, bool draw_debug)
{
	int first_hit = pile.hit_count > RECENT_HITS_MAX ? pile.hit_count - RECENT_HITS_MAX : 0;
	int first_blast = pile.blast_count > RECENT_BLASTS_MAX ? pile.blast_count - RECENT_BLASTS_MAX : 0;
	int i;

	soft_render_begin(renderer, camera, SKY_COLOR);
	// The pieces first, their backs not drawn (they are closed), then
	// the ground, of which only what shows between them is filled
	renderer->cull_back_faces = true;
	for (i = 0; i < pile.piece_count; i++)
	{
		soft_render_set_object(renderer, (uint32_t)i + 2);
		draw_piece(renderer, &pile.pieces[i]);
	}
	renderer->cull_back_faces = false;
	soft_render_set_object(renderer, 1);
	soft_render_ground(renderer, 0.0f, 0.0f, 0.0f, 40.0f, 1.0f, GROUND_COLOR_FIRST, GROUND_COLOR_SECOND);
	if (draw_outlines)
	{
		soft_render_outlines(renderer);
	}

	if (draw_debug)
	{
		int point_count;
		float largest_impulse;

		visit_contacts(renderer, &point_count, &largest_impulse);
	}

	// The hits of the last frames, each with how fast the two came together
	for (i = first_hit; i < pile.hit_count; i++)
	{
		const hit_t *hit = &pile.recent_hits[i % RECENT_HITS_MAX];

		if (hit->frame_index <= pile.frame_index && pile.frame_index - hit->frame_index < HIT_SHOWN_FRAMES)
		{
			soft_render_point(renderer, vector_floats(&hit->point), 7, HIT_COLOR);
			if (draw_debug)
			{
				char text[32];

				snprintf(text, sizeof(text), "%.1f M/S", (double)hit->approach_speed);
				draw_label(renderer, hit->point, text);
			}
		}
	}
	for (i = first_blast; i < pile.blast_count; i++)
	{
		const blast_mark_t *mark = &pile.recent_blasts[i % RECENT_BLASTS_MAX];

		if (mark->frame_index <= pile.frame_index && pile.frame_index - mark->frame_index < BLAST_SHOWN_FRAMES)
		{
			soft_render_point(renderer, vector_floats(&mark->position), 11, IMPULSE_COLOR);
			draw_label(renderer, mark->position, "BLAST");
		}
	}
}

static int count_bodies(void)
{
	int body_count = 0;
	int i;

	for (i = 0; i < pile.piece_count; i++)
	{
		if (piece_starts_body(i))
		{
			body_count++;
		}
	}
	return body_count;
}

static void pile_scene_describe(char *text, int text_size)
{
	int point_count;
	float largest_impulse;

	visit_contacts(NULL, &point_count, &largest_impulse);
	snprintf(text, (size_t)text_size, "%d BODIES, %d AWAKE  %d CONTACT POINTS, LARGEST %.0f NS  %d HITS, FASTEST %.1f M/S",
		count_bodies(), b3World_GetAwakeBodyCount(pile.world), point_count, (double)largest_impulse, pile.hit_count, (double)pile.fastest_hit_speed);
}

// (FNV-1a over the bits of every piece's body's place and turn)
static uint32_t pile_scene_checksum(void)
{
	uint32_t hash = 2166136261u;
	int i;
	size_t k;

	for (i = 0; i < pile.piece_count; i++)
	{
		b3WorldTransform body_transform = b3Body_GetTransform(pile.pieces[i].body);
		float values[7] =
		{
			body_transform.p.x, body_transform.p.y, body_transform.p.z,
			body_transform.q.v.x, body_transform.q.v.y, body_transform.q.v.z, body_transform.q.s
		};
		const unsigned char *bytes = (const unsigned char *)values;

		for (k = 0; k < sizeof(values); k++)
		{
			hash = (hash ^ bytes[k]) * 16777619u;
		}
	}
	return hash;
}

const damage_scene_t pile_scene =
{
	.name = "pile",
	.about = "Box3D alone: a wall, stacked and falling logs, a body split in the air, a ball, a blast",
	.open = pile_scene_open,
	.close = pile_scene_close,
	.default_camera = pile_scene_default_camera,
	.seek = pile_scene_seek,
	.draw = pile_scene_draw,
	.describe = pile_scene_describe,
	.checksum = pile_scene_checksum,
	.cast_ray = pile_scene_cast_ray,
	.inspect = NULL,
};

const damage_scene_t sandbox_scene =
{
	.name = "sandbox",
	.about = "the wall and the stacked logs left alone: throw balls and set off blasts at them",
	.open = sandbox_scene_open,
	.close = pile_scene_close,
	.default_camera = pile_scene_default_camera,
	.seek = pile_scene_seek,
	.draw = pile_scene_draw,
	.describe = pile_scene_describe,
	.checksum = pile_scene_checksum,
	.cast_ray = pile_scene_cast_ray,
	.inspect = NULL,
};
