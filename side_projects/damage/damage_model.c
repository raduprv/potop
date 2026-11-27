// The damage model: see damage_model.h

#include "damage_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "damage_ini.h"

#define PI 3.14159265358979f
// A log is a prism of this many flat sides, lying on a flat one (as in
// the pile scene: Box3D has no cylinder)
#define LOG_SIDE_COUNT 12
#define LOG_FIRST_CORNER_RADIANS (PI / (float)LOG_SIDE_COUNT)
#define FRICTION 0.6f
// A piece's group while the islands are being found
#define GROUP_UNLABELED -2
#define GRAVITY 9.81f
// A link under or over a piece's middle by less than this is at its side
#define LEVEL_METERS 0.01f
// The support pass: it stops when what is left unbalanced is this small a
// part of the weight, or after this many rounds
#define SUPPORT_TOLERANCE 1e-5
#define SUPPORT_ROUNDS_MAX 4000
#define RESTORED_BODIES_MAX 256

#define BROKEN_SHOWN_FRAMES 90
#define HIT_SHOWN_FRAMES 30
#define ASLEEP_COLOR 0x707070
#define PROJECTILE_COLOR 0x505868
#define LINK_WHOLE_COLOR 0x30E040
#define LINK_HURT_COLOR 0xFFE040
#define LINK_BROKEN_COLOR 0xFF3020
#define LINK_OVERLOADED_COLOR 0xFF8000
#define CRUSHED_COLOR 0x303030
#define HIT_COLOR 0xFF2020
#define LABEL_COLOR 0xFFFFFF

// Scratch for walking the graph (one walk at a time: nothing here is
// used from two threads)
static int walk_queue[DAMAGE_PIECES_MAX];
static float walk_amounts[DAMAGE_PIECES_MAX];
static int walk_piece_stamps[DAMAGE_PIECES_MAX];
static int walk_link_stamps[DAMAGE_LINKS_MAX];
static int walk_stamp;
static int group_members[DAMAGE_PIECES_MAX];
static int group_parts[DAMAGE_PIECES_MAX];
// The support pass's unknowns: six a standing piece (how far it moved
// along x, y, z, how far it turned about them), -1 for one that does not
// stand; and the vectors of its solver
static int support_unknowns[DAMAGE_PIECES_MAX];
static double solver_x[DAMAGE_PIECES_MAX * 6];
static double solver_load[DAMAGE_PIECES_MAX * 6];
static double solver_residual[DAMAGE_PIECES_MAX * 6];
static double solver_scaled[DAMAGE_PIECES_MAX * 6];
static double solver_direction[DAMAGE_PIECES_MAX * 6];
static double solver_product[DAMAGE_PIECES_MAX * 6];
static double solver_diagonal[DAMAGE_PIECES_MAX * 6];
static float piece_compressions[DAMAGE_PIECES_MAX];
static b3Vec3 blast_impulses[DAMAGE_PIECES_MAX];
static b3Vec3 blast_points[DAMAGE_PIECES_MAX];

static b3Vec3 make_vector(float x, float y, float z)
{
	b3Vec3 vector = {x, y, z};

	return vector;
}

static const float *vector_floats(const b3Vec3 *vector)
{
	return (const float *)vector;
}

static const float *rotation_floats(const b3Quat *rotation)
{
	return (const float *)rotation;
}

// ---------------------------------------------------------------------
// Settings

static void set_link_defaults(damage_link_settings_t *link, float threshold, float hit_points, float compression, float pull, float cantilever)
{
	link->threshold = threshold;
	link->hit_points = hit_points;
	link->compression = compression;
	link->pull = pull;
	link->cantilever = cantilever;
}

void damage_settings_default(damage_settings_t *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->densities[DAMAGE_MATERIAL_WOOD] = 600.0f;
	settings->densities[DAMAGE_MATERIAL_BRICK] = 1800.0f;
	settings->densities[DAMAGE_MATERIAL_GLASS] = 2500.0f;
	settings->crush[DAMAGE_MATERIAL_WOOD] = 300000.0f;
	settings->crush[DAMAGE_MATERIAL_BRICK] = 500000.0f;
	settings->crush[DAMAGE_MATERIAL_GLASS] = 9000.0f;
	set_link_defaults(&settings->links[DAMAGE_LINK_NAILS], 150.0f, 600.0f, 200000.0f, 4000.0f, 2500.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_MORTAR], 60.0f, 200.0f, 500000.0f, 3000.0f, 900.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_FOUNDATION], 300.0f, 1500.0f, 1000000.0f, 5000.0f, 5000.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_GLAZING], 30.0f, 60.0f, 100000.0f, 500.0f, 200.0f);
	settings->hop_falloff = 0.5f;
	settings->hit_speed = 1.0f;
	settings->rubble_pieces = 3;
	settings->breakthrough_speed_kept = 0.85f;
	settings->blast_radius = 1.5f;
	settings->blast_falloff = 3.0f;
	settings->collapse_seconds = 1.5f;
	settings->joint_arm = 0.15f;
	settings->shape_gap = 0.003f;
	settings->ball_radius = 0.2f;
	settings->ball_density = 7800.0f;
}

static bool read_float(const char *value, float *result)
{
	return sscanf(value, "%f", result) == 1;
}

static bool read_link_key(damage_link_settings_t *link, const char *key, const char *value)
{
	if (strcmp(key, "threshold") == 0)
	{
		return read_float(value, &link->threshold);
	}
	if (strcmp(key, "hit_points") == 0)
	{
		return read_float(value, &link->hit_points);
	}
	if (strcmp(key, "compression") == 0)
	{
		return read_float(value, &link->compression);
	}
	if (strcmp(key, "pull") == 0)
	{
		return read_float(value, &link->pull);
	}
	if (strcmp(key, "cantilever") == 0)
	{
		return read_float(value, &link->cantilever);
	}
	return false;
}

static bool read_material_key(damage_settings_t *settings, damage_material_t material, const char *key, const char *value)
{
	if (strcmp(key, "density") == 0)
	{
		return read_float(value, &settings->densities[material]);
	}
	if (strcmp(key, "crush") == 0)
	{
		return read_float(value, &settings->crush[material]);
	}
	return false;
}

static bool read_settings_key(void *user, const char *section, const char *key, const char *value)
{
	damage_settings_t *settings = user;

	if (strcmp(section, "model") == 0)
	{
		if (strcmp(key, "hop_falloff") == 0)
		{
			return read_float(value, &settings->hop_falloff);
		}
		if (strcmp(key, "hit_speed") == 0)
		{
			return read_float(value, &settings->hit_speed);
		}
		if (strcmp(key, "rubble_pieces") == 0)
		{
			return sscanf(value, "%d", &settings->rubble_pieces) == 1;
		}
		if (strcmp(key, "breakthrough_speed_kept") == 0)
		{
			return read_float(value, &settings->breakthrough_speed_kept);
		}
		if (strcmp(key, "blast_radius") == 0)
		{
			return read_float(value, &settings->blast_radius);
		}
		if (strcmp(key, "blast_falloff") == 0)
		{
			return read_float(value, &settings->blast_falloff);
		}
		if (strcmp(key, "shape_gap") == 0)
		{
			return read_float(value, &settings->shape_gap);
		}
		if (strcmp(key, "collapse_seconds") == 0)
		{
			return read_float(value, &settings->collapse_seconds);
		}
		if (strcmp(key, "joint_arm") == 0)
		{
			return read_float(value, &settings->joint_arm);
		}
		return false;
	}
	if (strcmp(section, "ball") == 0)
	{
		if (strcmp(key, "radius") == 0)
		{
			return read_float(value, &settings->ball_radius);
		}
		if (strcmp(key, "density") == 0)
		{
			return read_float(value, &settings->ball_density);
		}
		return false;
	}
	if (strcmp(section, "wood") == 0)
	{
		return read_material_key(settings, DAMAGE_MATERIAL_WOOD, key, value);
	}
	if (strcmp(section, "brick") == 0)
	{
		return read_material_key(settings, DAMAGE_MATERIAL_BRICK, key, value);
	}
	if (strcmp(section, "glass") == 0)
	{
		return read_material_key(settings, DAMAGE_MATERIAL_GLASS, key, value);
	}
	if (strcmp(section, "nails") == 0)
	{
		return read_link_key(&settings->links[DAMAGE_LINK_NAILS], key, value);
	}
	if (strcmp(section, "mortar") == 0)
	{
		return read_link_key(&settings->links[DAMAGE_LINK_MORTAR], key, value);
	}
	if (strcmp(section, "foundation") == 0)
	{
		return read_link_key(&settings->links[DAMAGE_LINK_FOUNDATION], key, value);
	}
	if (strcmp(section, "glazing") == 0)
	{
		return read_link_key(&settings->links[DAMAGE_LINK_GLAZING], key, value);
	}
	return false;
}

bool damage_settings_load(damage_settings_t *settings, const char *path)
{
	return damage_ini_read(path, read_settings_key, settings);
}

// ---------------------------------------------------------------------
// Building

void damage_model_create(damage_model_t *model, b3WorldId world, const damage_settings_t *settings)
{
	memset(model, 0, sizeof(*model));
	model->world = world;
	model->settings = *settings;
}

static int add_piece(damage_model_t *model, damage_piece_shape_t shape, b3Vec3 position, b3Quat rotation, damage_material_t material, uint32_t color)
{
	damage_piece_t *piece;

	if (model->finished || model->piece_count >= DAMAGE_PIECES_MAX)
	{
		return -1;
	}
	piece = &model->pieces[model->piece_count];
	memset(piece, 0, sizeof(*piece));
	piece->shape = shape;
	piece->material = material;
	piece->local.p = position;
	piece->local.q = rotation;
	piece->group = GROUP_UNLABELED;
	piece->color = color;
	return model->piece_count++;
}

int damage_model_add_box(damage_model_t *model, b3Vec3 position, b3Quat rotation, b3Vec3 half_extents, damage_material_t material, uint32_t color)
{
	int piece_index = add_piece(model, DAMAGE_PIECE_BOX, position, rotation, material, color);

	if (piece_index >= 0)
	{
		model->pieces[piece_index].half_extents = half_extents;
		model->pieces[piece_index].mass = 8.0f * half_extents.x * half_extents.y * half_extents.z * model->settings.densities[material];
	}
	return piece_index;
}

int damage_model_add_log(damage_model_t *model, b3Vec3 position, b3Quat rotation, float radius, float half_length, damage_material_t material, uint32_t color)
{
	int piece_index = add_piece(model, DAMAGE_PIECE_LOG, position, rotation, material, color);

	if (piece_index >= 0)
	{
		// (The prism's cross section: 12 triangles of two sides a radius,
		// 30 degrees between them, 3 radius squared in all)
		model->pieces[piece_index].radius = radius;
		model->pieces[piece_index].half_length = half_length;
		model->pieces[piece_index].mass = 3.0f * radius * radius * 2.0f * half_length * model->settings.densities[material];
	}
	return piece_index;
}

// Which way a link's joint faces, from piece_b towards piece_a (see
// damage_model.h)
static b3Vec3 link_normal(const damage_model_t *model, int piece_a, int piece_b, b3Vec3 point)
{
	b3Vec3 from_point = b3Sub(model->pieces[piece_a].local.p, point);
	b3Vec3 level;
	float length;

	if (piece_b == DAMAGE_GROUND)
	{
		return make_vector(0.0f, 1.0f, 0.0f);
	}
	if (from_point.y > LEVEL_METERS && point.y - model->pieces[piece_b].local.p.y > LEVEL_METERS)
	{
		return make_vector(0.0f, 1.0f, 0.0f);
	}
	if (from_point.y < -LEVEL_METERS && point.y - model->pieces[piece_b].local.p.y < -LEVEL_METERS)
	{
		return make_vector(0.0f, -1.0f, 0.0f);
	}
	level = make_vector(from_point.x, 0.0f, from_point.z);
	length = b3Length(level);
	if (length < 1e-4f)
	{
		// Right over or under piece_a's middle after all
		return make_vector(0.0f, from_point.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
	}
	return b3MulSV(1.0f / length, level);
}

int damage_model_add_link(damage_model_t *model, int piece_a, int piece_b, b3Vec3 point, damage_link_type_t type)
{
	damage_link_t *link;

	if (model->finished || model->link_count >= DAMAGE_LINKS_MAX || piece_a < 0 || piece_a >= model->piece_count
		|| piece_b >= model->piece_count || (piece_b < 0 && piece_b != DAMAGE_GROUND) || piece_a == piece_b)
	{
		return -1;
	}
	link = &model->links[model->link_count];
	memset(link, 0, sizeof(*link));
	link->piece_a = piece_a;
	link->piece_b = piece_b;
	link->type = type;
	link->local_point = point;
	link->local_normal = link_normal(model, piece_a, piece_b, point);
	link->hit_points_left = model->settings.links[type].hit_points;
	return model->link_count++;
}

// A log's collision shape: a prism along x (the world keeps its own copy
// of a hull given to a shape, so the caller destroys this one)
static b3HullData *create_log_hull(float radius, float half_length)
{
	b3Vec3 points[LOG_SIDE_COUNT * 2];
	int i;

	for (i = 0; i < LOG_SIDE_COUNT; i++)
	{
		float angle = LOG_FIRST_CORNER_RADIANS + 2.0f * PI * (float)i / (float)LOG_SIDE_COUNT;
		float y = radius * cosf(angle);
		float z = radius * sinf(angle);

		points[i * 2] = make_vector(-half_length, y, z);
		points[i * 2 + 1] = make_vector(half_length, y, z);
	}
	return b3CreateHull(points, LOG_SIDE_COUNT * 2, LOG_SIDE_COUNT * 2);
}

// A piece's shape on a body, at the piece's place in the building's
// frame. The shape's user data is the piece's number plus one.
//
// The shape is a little smaller than the piece (shape_gap): pieces that
// touch in the building have no play at all between them, and one that
// comes loose among others still standing would be held like a drawer
// gone askew, whatever pushes it.
static void add_piece_shape(const damage_model_t *model, b3BodyId body, int piece_index)
{
	const damage_piece_t *piece = &model->pieces[piece_index];
	b3ShapeDef definition = b3DefaultShapeDef();
	float gap = model->settings.shape_gap;

	definition.density = model->settings.densities[piece->material];
	definition.baseMaterial.friction = FRICTION;
	definition.enableHitEvents = true;
	definition.userData = (void *)(intptr_t)(piece_index + 1);
	if (piece->shape == DAMAGE_PIECE_BOX)
	{
		b3BoxHull hull = b3MakeBoxHull(piece->half_extents.x - gap, piece->half_extents.y - gap, piece->half_extents.z - gap);

		b3CreateTransformedHullShape(body, &definition, &hull.base, piece->local, b3Vec3_one);
	}
	else
	{
		b3HullData *hull = create_log_hull(piece->radius - gap, piece->half_length - gap);

		if (hull != NULL)
		{
			b3CreateTransformedHullShape(body, &definition, hull, piece->local, b3Vec3_one);
			b3DestroyHull(hull);
		}
	}
}

// The body of a group: every piece of it a shape, the body where the
// building's frame is. Its user data is the group's number plus one.
static void create_group_body(damage_model_t *model, int group_index, b3Transform frame)
{
	damage_group_t *group = &model->groups[group_index];
	b3BodyDef definition = b3DefaultBodyDef();
	int i;

	definition.type = group->anchored ? b3_staticBody : b3_dynamicBody;
	definition.position = frame.p;
	definition.rotation = frame.q;
	definition.userData = (void *)(intptr_t)(group_index + 1);
	group->body = b3CreateBody(model->world, &definition);
	for (i = 0; i < model->piece_count; i++)
	{
		if (model->pieces[i].group == group_index)
		{
			add_piece_shape(model, group->body, i);
		}
	}
}

// The lowest free group (the same choice every run)
static int allocate_group(damage_model_t *model)
{
	int i;

	for (i = 0; i < DAMAGE_PIECES_MAX; i++)
	{
		if (!model->groups[i].used)
		{
			memset(&model->groups[i], 0, sizeof(model->groups[i]));
			model->groups[i].used = true;
			return i;
		}
	}
	return -1;
}

// Gives the group to every unlabeled piece a walk over whole links
// reaches from one, and fills in the group's count, whether a whole link
// holds it to the ground, and whether it is rubble
static void label_island(damage_model_t *model, int first_piece, int group_index)
{
	damage_group_t *group = &model->groups[group_index];
	int queue_start = 0;
	int queue_end = 0;
	int k;

	model->pieces[first_piece].group = group_index;
	walk_queue[queue_end++] = first_piece;
	while (queue_start < queue_end)
	{
		int piece_index = walk_queue[queue_start++];

		group->piece_count++;
		for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
		{
			const damage_link_t *link = &model->links[model->piece_links[k]];
			int other;

			if (link->broken)
			{
				continue;
			}
			if (link->piece_b == DAMAGE_GROUND)
			{
				group->anchored = true;
				continue;
			}
			other = link->piece_a == piece_index ? link->piece_b : link->piece_a;
			if (model->pieces[other].group == GROUP_UNLABELED)
			{
				model->pieces[other].group = group_index;
				walk_queue[queue_end++] = other;
			}
		}
	}
	group->rubble = !group->anchored && group->piece_count < model->settings.rubble_pieces;
}

void damage_model_finish(damage_model_t *model)
{
	int fill[DAMAGE_PIECES_MAX];
	int i;

	if (model->finished)
	{
		return;
	}
	model->finished = true;
	model->support_dirty = true;

	// Every piece's links, one after another
	memset(model->piece_link_start, 0, sizeof(model->piece_link_start));
	for (i = 0; i < model->link_count; i++)
	{
		model->piece_link_start[model->links[i].piece_a + 1]++;
		if (model->links[i].piece_b != DAMAGE_GROUND)
		{
			model->piece_link_start[model->links[i].piece_b + 1]++;
		}
	}
	for (i = 0; i < model->piece_count; i++)
	{
		model->piece_link_start[i + 1] += model->piece_link_start[i];
		fill[i] = model->piece_link_start[i];
	}
	for (i = 0; i < model->link_count; i++)
	{
		model->piece_links[fill[model->links[i].piece_a]++] = i;
		if (model->links[i].piece_b != DAMAGE_GROUND)
		{
			model->piece_links[fill[model->links[i].piece_b]++] = i;
		}
	}

	for (i = 0; i < model->piece_count; i++)
	{
		if (model->pieces[i].group == GROUP_UNLABELED)
		{
			int group_index = allocate_group(model);

			label_island(model, i, group_index);
			create_group_body(model, group_index, b3Transform_identity);
		}
	}
}

void damage_model_add_projectile(damage_model_t *model, b3BodyId body, float radius)
{
	damage_projectile_t *projectile;

	if (model->projectile_count >= DAMAGE_PROJECTILES_MAX)
	{
		return;
	}
	projectile = &model->projectiles[model->projectile_count];
	memset(projectile, 0, sizeof(*projectile));
	projectile->body = body;
	projectile->radius = radius;
	// (Negative: not a group)
	b3Body_SetUserData(body, (void *)(intptr_t)(-(model->projectile_count + 1)));
	model->projectile_count++;
}

// ---------------------------------------------------------------------
// Groups made anew

// One group after links of it broke: its islands found again. If it is
// still one island on the same footing nothing changes; else its body is
// destroyed and each island gets its own, where the old one was and
// moving as that part of it did.
static void rebuild_group(damage_model_t *model, int old_group_index)
{
	damage_group_t old_group = model->groups[old_group_index];
	b3Transform frame = b3Body_GetTransform(old_group.body);
	b3Vec3 old_center = b3Body_GetWorldCenter(old_group.body);
	b3Vec3 old_linear_velocity = b3Body_GetLinearVelocity(old_group.body);
	b3Vec3 old_angular_velocity = b3Body_GetAngularVelocity(old_group.body);
	int member_count = 0;
	int part_count = 0;
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		if (model->pieces[i].group == old_group_index)
		{
			model->pieces[i].group = GROUP_UNLABELED;
			group_members[member_count++] = i;
		}
	}
	model->groups[old_group_index].used = false;
	for (i = 0; i < member_count; i++)
	{
		if (model->pieces[group_members[i]].group == GROUP_UNLABELED)
		{
			int group_index = allocate_group(model);

			label_island(model, group_members[i], group_index);
			group_parts[part_count++] = group_index;
		}
	}

	if (part_count == 1 && model->groups[group_parts[0]].anchored == old_group.anchored)
	{
		// The same pieces on the same footing: the body stays
		damage_group_t *group = &model->groups[group_parts[0]];

		group->body = old_group.body;
		group->rubble = old_group.rubble;
		group->linear_velocity_before_step = old_group.linear_velocity_before_step;
		group->angular_velocity_before_step = old_group.angular_velocity_before_step;
		b3Body_SetUserData(group->body, (void *)(intptr_t)(group_parts[0] + 1));
		return;
	}

	b3DestroyBody(old_group.body);
	for (i = 0; i < part_count; i++)
	{
		damage_group_t *group = &model->groups[group_parts[i]];

		create_group_body(model, group_parts[i], frame);
		group->rebuilt = true;
		if (!group->anchored && !old_group.anchored)
		{
			// Its own center goes as that point of the old body went
			b3Vec3 center = b3Body_GetWorldCenter(group->body);

			b3Body_SetLinearVelocity(group->body, b3Add(old_linear_velocity, b3Cross(old_angular_velocity, b3Sub(center, old_center))));
			b3Body_SetAngularVelocity(group->body, old_angular_velocity);
		}
	}
}

static void rebuild_dirty_groups(damage_model_t *model)
{
	int i;

	if (!model->any_group_dirty)
	{
		return;
	}
	model->any_group_dirty = false;
	// (A group made while this runs takes a free number: it is never dirty)
	for (i = 0; i < DAMAGE_PIECES_MAX; i++)
	{
		if (model->group_dirty[i])
		{
			model->group_dirty[i] = false;
			if (model->groups[i].used)
			{
				rebuild_group(model, i);
			}
		}
	}
}

// ---------------------------------------------------------------------
// Damage

static void break_link(damage_model_t *model, int link_index, int frame_index)
{
	damage_link_t *link = &model->links[link_index];

	if (link->broken)
	{
		return;
	}
	link->hit_points_left = 0.0f;
	link->broken = true;
	link->broken_frame = frame_index;
	model->broken_link_count++;
	model->group_dirty[model->pieces[link->piece_a].group] = true;
	model->any_group_dirty = true;
	model->support_dirty = true;
}

// A share of an impulse on a link. True when that broke it.
static bool hurt_link(damage_model_t *model, int link_index, float share, int frame_index)
{
	damage_link_t *link = &model->links[link_index];
	float threshold = model->settings.links[link->type].threshold;

	if (link->broken || share <= threshold)
	{
		return false;
	}
	link->hit_points_left -= share - threshold;
	if (link->hit_points_left > 0.0f)
	{
		return false;
	}
	break_link(model, link_index, frame_index);
	return true;
}

// An impulse on a piece: shared evenly among its whole links; each link
// passes what it got, times hop_falloff, to the piece at its other end,
// which shares that among its own. True when a link broke.
static bool damage_piece(damage_model_t *model, int piece_index, float impulse, int frame_index)
{
	float smallest_threshold = model->settings.links[0].threshold;
	bool broke = false;
	int queue_start = 0;
	int queue_end = 0;
	int k;

	if (model->pieces[piece_index].group == DAMAGE_GONE || model->groups[model->pieces[piece_index].group].rubble)
	{
		return false;
	}
	for (k = 1; k < DAMAGE_LINK_TYPE_COUNT; k++)
	{
		if (model->settings.links[k].threshold < smallest_threshold)
		{
			smallest_threshold = model->settings.links[k].threshold;
		}
	}

	walk_stamp++;
	walk_piece_stamps[piece_index] = walk_stamp;
	walk_amounts[piece_index] = impulse;
	walk_queue[queue_end++] = piece_index;
	while (queue_start < queue_end)
	{
		int current = walk_queue[queue_start++];
		int first_link = model->piece_link_start[current];
		int end_link = model->piece_link_start[current + 1];
		int whole_count = 0;
		float share;

		for (k = first_link; k < end_link; k++)
		{
			if (!model->links[model->piece_links[k]].broken)
			{
				whole_count++;
			}
		}
		if (whole_count == 0)
		{
			continue;
		}
		share = walk_amounts[current] / (float)whole_count;
		for (k = first_link; k < end_link; k++)
		{
			int link_index = model->piece_links[k];
			const damage_link_t *link = &model->links[link_index];
			int other;
			float passed_on = share * model->settings.hop_falloff;

			// (A link is hurt once a hit, from the side reached first)
			if (link->broken || walk_link_stamps[link_index] == walk_stamp)
			{
				continue;
			}
			walk_link_stamps[link_index] = walk_stamp;
			if (hurt_link(model, link_index, share, frame_index))
			{
				broke = true;
			}
			if (link->piece_b == DAMAGE_GROUND)
			{
				continue;
			}
			other = link->piece_a == current ? link->piece_b : link->piece_a;
			if (walk_piece_stamps[other] != walk_stamp && passed_on > smallest_threshold)
			{
				walk_piece_stamps[other] = walk_stamp;
				walk_amounts[other] = passed_on;
				walk_queue[queue_end++] = other;
			}
		}
	}
	return broke;
}

// ---------------------------------------------------------------------
// What holds what up

// What a link does between its two pieces, given where every standing
// piece has moved and turned to (the unknowns): how far apart the two
// have come at the link, and how far they have turned apart. With the
// spring's stiffness taken as 1 these are also the force through the
// link and, times the square of joint_arm, its moment.
static void link_strain(const damage_model_t *model, const damage_link_t *link, const double *unknowns, double apart[3], double turned[3])
{
	int a = support_unknowns[link->piece_a] * 6;
	const b3Vec3 *center_a = &model->pieces[link->piece_a].local.p;
	double arm_x = (double)link->local_point.x - (double)center_a->x;
	double arm_y = (double)link->local_point.y - (double)center_a->y;
	double arm_z = (double)link->local_point.z - (double)center_a->z;

	// The link's point on a piece moves by the piece's move plus its turn
	// crossed with the arm from the piece's middle
	apart[0] = unknowns[a + 0] + unknowns[a + 4] * arm_z - unknowns[a + 5] * arm_y;
	apart[1] = unknowns[a + 1] + unknowns[a + 5] * arm_x - unknowns[a + 3] * arm_z;
	apart[2] = unknowns[a + 2] + unknowns[a + 3] * arm_y - unknowns[a + 4] * arm_x;
	turned[0] = unknowns[a + 3];
	turned[1] = unknowns[a + 4];
	turned[2] = unknowns[a + 5];
	if (link->piece_b != DAMAGE_GROUND)
	{
		int b = support_unknowns[link->piece_b] * 6;
		const b3Vec3 *center_b = &model->pieces[link->piece_b].local.p;

		arm_x = (double)link->local_point.x - (double)center_b->x;
		arm_y = (double)link->local_point.y - (double)center_b->y;
		arm_z = (double)link->local_point.z - (double)center_b->z;
		apart[0] -= unknowns[b + 0] + unknowns[b + 4] * arm_z - unknowns[b + 5] * arm_y;
		apart[1] -= unknowns[b + 1] + unknowns[b + 5] * arm_x - unknowns[b + 3] * arm_z;
		apart[2] -= unknowns[b + 2] + unknowns[b + 3] * arm_y - unknowns[b + 4] * arm_x;
		turned[0] -= unknowns[b + 3];
		turned[1] -= unknowns[b + 4];
		turned[2] -= unknowns[b + 5];
	}
}

// The force and the turning force every standing piece gets from all its
// links when the pieces are where the unknowns say: what the solver has
// to make equal to the pieces' weights
static void springs_product(const damage_model_t *model, const double *unknowns, double *product, int unknown_count)
{
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	int i;
	int k;

	for (i = 0; i < unknown_count; i++)
	{
		product[i] = 0.0;
	}
	for (i = 0; i < model->link_count; i++)
	{
		const damage_link_t *link = &model->links[i];
		double apart[3];
		double turned[3];
		int side;

		if (link->broken || support_unknowns[link->piece_a] < 0)
		{
			continue;
		}
		link_strain(model, link, unknowns, apart, turned);
		// On piece a the spring's force, and its turning force about the
		// piece's middle (the arm crossed with the force, and the spring's
		// own resistance to turning); on piece b the opposite
		for (side = 0; side < 2; side++)
		{
			int piece_index = side == 0 ? link->piece_a : link->piece_b;
			double sign = side == 0 ? 1.0 : -1.0;
			const b3Vec3 *center;
			double arm_x;
			double arm_y;
			double arm_z;
			int first;

			if (piece_index == DAMAGE_GROUND)
			{
				continue;
			}
			center = &model->pieces[piece_index].local.p;
			arm_x = (double)link->local_point.x - (double)center->x;
			arm_y = (double)link->local_point.y - (double)center->y;
			arm_z = (double)link->local_point.z - (double)center->z;
			first = support_unknowns[piece_index] * 6;
			for (k = 0; k < 3; k++)
			{
				product[first + k] += sign * apart[k];
			}
			product[first + 3] += sign * (arm_y * apart[2] - arm_z * apart[1] + turn_stiffness * turned[0]);
			product[first + 4] += sign * (arm_z * apart[0] - arm_x * apart[2] + turn_stiffness * turned[1]);
			product[first + 5] += sign * (arm_x * apart[1] - arm_y * apart[0] + turn_stiffness * turned[2]);
		}
	}
}

// The support pass: where everything that stands settles under its own
// weight with every link a spring (see damage_model.h), found by
// conjugate gradients from where it settled last time; then what every
// link bears against what it can. Pieces pressed harder than their
// material bears are crushed: their links break.
static void work_out_support(damage_model_t *model, int frame_index)
{
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	int queue_start = 0;
	int queue_end = 0;
	int unknown_count;
	double load_squared = 0.0;
	double residual_scaled = 0.0;
	int round = 0;
	int i;
	int k;

	model->support_dirty = false;
	model->overloaded_count = 0;
	for (i = 0; i < model->link_count; i++)
	{
		model->links[i].overload = 0.0f;
		model->links[i].static_compression = 0.0f;
	}

	// What stands: every piece a walk over whole links reaches from one a
	// whole link holds to the ground. The rest is loose, Box3D's business.
	for (i = 0; i < model->piece_count; i++)
	{
		support_unknowns[i] = -1;
	}
	for (i = 0; i < model->link_count; i++)
	{
		const damage_link_t *link = &model->links[i];

		if (!link->broken && link->piece_b == DAMAGE_GROUND && support_unknowns[link->piece_a] < 0)
		{
			support_unknowns[link->piece_a] = queue_end;
			walk_queue[queue_end++] = link->piece_a;
		}
	}
	while (queue_start < queue_end)
	{
		int piece_index = walk_queue[queue_start++];

		for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
		{
			const damage_link_t *link = &model->links[model->piece_links[k]];
			int other;

			if (link->broken || link->piece_b == DAMAGE_GROUND)
			{
				continue;
			}
			other = link->piece_a == piece_index ? link->piece_b : link->piece_a;
			if (support_unknowns[other] < 0)
			{
				support_unknowns[other] = queue_end;
				walk_queue[queue_end++] = other;
			}
		}
	}
	unknown_count = queue_end * 6;
	for (i = 0; i < model->piece_count; i++)
	{
		if (support_unknowns[i] < 0)
		{
			// Forgotten: if it ever stood again it would start from rest
			memset(&model->support_solution[i * 6], 0, 6 * sizeof(double));
		}
	}
	if (unknown_count == 0)
	{
		model->support_iterations = 0;
		return;
	}

	// The weights; where the pieces settled last time; and how stiffly
	// each unknown is held by its own links alone, which the solver
	// divides by to be at home with turns and moves alike
	for (i = 0; i < queue_end; i++)
	{
		int piece_index = walk_queue[i];

		for (k = 0; k < 6; k++)
		{
			solver_load[i * 6 + k] = 0.0;
			solver_x[i * 6 + k] = model->support_solution[piece_index * 6 + k];
			solver_diagonal[i * 6 + k] = 0.0;
		}
		solver_load[i * 6 + 1] = -(double)model->pieces[piece_index].mass * (double)GRAVITY;
		load_squared += solver_load[i * 6 + 1] * solver_load[i * 6 + 1];
	}
	for (i = 0; i < model->link_count; i++)
	{
		const damage_link_t *link = &model->links[i];
		int side;

		if (link->broken || support_unknowns[link->piece_a] < 0)
		{
			continue;
		}
		for (side = 0; side < 2; side++)
		{
			int piece_index = side == 0 ? link->piece_a : link->piece_b;
			const b3Vec3 *center;
			double arm_x;
			double arm_y;
			double arm_z;
			int first;

			if (piece_index == DAMAGE_GROUND)
			{
				continue;
			}
			center = &model->pieces[piece_index].local.p;
			arm_x = (double)link->local_point.x - (double)center->x;
			arm_y = (double)link->local_point.y - (double)center->y;
			arm_z = (double)link->local_point.z - (double)center->z;
			first = support_unknowns[piece_index] * 6;
			solver_diagonal[first + 0] += 1.0;
			solver_diagonal[first + 1] += 1.0;
			solver_diagonal[first + 2] += 1.0;
			solver_diagonal[first + 3] += turn_stiffness + arm_y * arm_y + arm_z * arm_z;
			solver_diagonal[first + 4] += turn_stiffness + arm_x * arm_x + arm_z * arm_z;
			solver_diagonal[first + 5] += turn_stiffness + arm_x * arm_x + arm_y * arm_y;
		}
	}

	// Conjugate gradients: what is still unbalanced, a direction to move
	// everything in that the earlier ones do not undo, as far along it
	// as balances best; until almost nothing is left
	springs_product(model, solver_x, solver_product, unknown_count);
	for (i = 0; i < unknown_count; i++)
	{
		solver_residual[i] = solver_load[i] - solver_product[i];
		solver_scaled[i] = solver_residual[i] / solver_diagonal[i];
		solver_direction[i] = solver_scaled[i];
		residual_scaled += solver_residual[i] * solver_scaled[i];
	}
	for (round = 0; round < SUPPORT_ROUNDS_MAX; round++)
	{
		double residual_squared = 0.0;
		double direction_product = 0.0;
		double step;
		double next_residual_scaled = 0.0;

		for (i = 0; i < unknown_count; i++)
		{
			residual_squared += solver_residual[i] * solver_residual[i];
		}
		if (residual_squared <= SUPPORT_TOLERANCE * SUPPORT_TOLERANCE * load_squared)
		{
			break;
		}
		springs_product(model, solver_direction, solver_product, unknown_count);
		for (i = 0; i < unknown_count; i++)
		{
			direction_product += solver_direction[i] * solver_product[i];
		}
		if (direction_product <= 0.0)
		{
			break;
		}
		step = residual_scaled / direction_product;
		for (i = 0; i < unknown_count; i++)
		{
			solver_x[i] += step * solver_direction[i];
			solver_residual[i] -= step * solver_product[i];
			solver_scaled[i] = solver_residual[i] / solver_diagonal[i];
			next_residual_scaled += solver_residual[i] * solver_scaled[i];
		}
		for (i = 0; i < unknown_count; i++)
		{
			solver_direction[i] = solver_scaled[i] + (next_residual_scaled / residual_scaled) * solver_direction[i];
		}
		residual_scaled = next_residual_scaled;
	}
	model->support_iterations = round;
	for (i = 0; i < queue_end; i++)
	{
		memcpy(&model->support_solution[walk_queue[i] * 6], &solver_x[i * 6], 6 * sizeof(double));
		piece_compressions[walk_queue[i]] = 0.0f;
	}

	// What every link bears: along its direction (pressed together or
	// pulled apart), across it, and turning; each against its limit
	for (i = 0; i < model->link_count; i++)
	{
		damage_link_t *link = &model->links[i];
		const damage_link_settings_t *limits = &model->settings.links[link->type];
		double apart[3];
		double turned[3];
		double along;
		double across_x;
		double across_y;
		double across_z;
		float compression;
		float pull;
		float sideways;
		float moment;
		float overload;

		if (link->broken || support_unknowns[link->piece_a] < 0)
		{
			continue;
		}
		link_strain(model, link, solver_x, apart, turned);
		along = apart[0] * (double)link->local_normal.x + apart[1] * (double)link->local_normal.y + apart[2] * (double)link->local_normal.z;
		across_x = apart[0] - along * (double)link->local_normal.x;
		across_y = apart[1] - along * (double)link->local_normal.y;
		across_z = apart[2] - along * (double)link->local_normal.z;
		// (piece_a come nearer piece_b: pressed together)
		compression = along < 0.0 ? (float)-along : 0.0f;
		pull = along > 0.0 ? (float)along : 0.0f;
		sideways = (float)sqrt(across_x * across_x + across_y * across_y + across_z * across_z);
		moment = (float)(turn_stiffness * sqrt(turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2]));

		// (What holds from the side is taken to bear what it bears pulled,
		// until sliding has limits of its own)
		overload = compression / limits->compression;
		if (pull / limits->pull > overload)
		{
			overload = pull / limits->pull;
		}
		if (sideways / limits->pull > overload)
		{
			overload = sideways / limits->pull;
		}
		if (moment / limits->cantilever > overload)
		{
			overload = moment / limits->cantilever;
		}
		link->overload = overload;
		link->static_compression = compression;
		if (overload > 1.0f)
		{
			model->overloaded_links[model->overloaded_count++] = i;
		}
		piece_compressions[link->piece_a] += compression;
		if (link->piece_b != DAMAGE_GROUND)
		{
			piece_compressions[link->piece_b] += compression;
		}
	}

	// A piece squeezed between what lies on it and what it lies on is
	// pressed from both sides: half of all that presses on it is what
	// goes through it
	for (i = 0; i < queue_end; i++)
	{
		int piece_index = walk_queue[i];

		if (piece_compressions[piece_index] * 0.5f > model->settings.crush[model->pieces[piece_index].material])
		{
			model->pieces[piece_index].crushed = true;
			for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
			{
				break_link(model, model->piece_links[k], frame_index);
			}
		}
	}
}

// The crushed pieces, by now each alone in a group of its own (their
// links broke in the support pass and the groups were made anew), are
// taken out of the world
static void remove_crushed(damage_model_t *model)
{
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		damage_piece_t *piece = &model->pieces[i];

		if (piece->crushed && piece->group != DAMAGE_GONE && model->groups[piece->group].piece_count == 1)
		{
			b3DestroyBody(model->groups[piece->group].body);
			model->groups[piece->group].used = false;
			piece->group = DAMAGE_GONE;
		}
	}
}

// The overloaded links give way a little more: each loses hit points as
// fast as it is overloaded (one at just its limit lasts
// collapse_seconds from whole)
static void give_way(damage_model_t *model, int frame_index, float step_seconds)
{
	int i;

	for (i = 0; i < model->overloaded_count; i++)
	{
		int link_index = model->overloaded_links[i];
		damage_link_t *link = &model->links[link_index];

		if (link->broken)
		{
			continue;
		}
		link->hit_points_left -= model->settings.links[link->type].hit_points * link->overload * step_seconds / model->settings.collapse_seconds;
		if (link->hit_points_left <= 0.0f)
		{
			break_link(model, link_index, frame_index);
		}
	}
}

void damage_model_before_step(damage_model_t *model)
{
	int i;

	for (i = 0; i < DAMAGE_PIECES_MAX; i++)
	{
		damage_group_t *group = &model->groups[i];

		if (group->used)
		{
			group->rebuilt = false;
			if (!group->anchored)
			{
				group->linear_velocity_before_step = b3Body_GetLinearVelocity(group->body);
				group->angular_velocity_before_step = b3Body_GetAngularVelocity(group->body);
			}
		}
	}
	for (i = 0; i < model->projectile_count; i++)
	{
		model->projectiles[i].linear_velocity_before_step = b3Body_GetLinearVelocity(model->projectiles[i].body);
		model->projectiles[i].angular_velocity_before_step = b3Body_GetAngularVelocity(model->projectiles[i].body);
	}
}

// A body whose hit broke links goes on with most of the speed it had
// before the step, instead of the bounce off a wall that was still whole
// when the step worked the hit out: the next step it meets what came
// loose. (A group made anew in this step is left as it was made.)
static void keep_going(damage_model_t *model, b3BodyId body)
{
	float kept = model->settings.breakthrough_speed_kept;
	intptr_t tag;

	if (!b3Body_IsValid(body))
	{
		return;
	}
	tag = (intptr_t)b3Body_GetUserData(body);
	if (tag > 0)
	{
		const damage_group_t *group = &model->groups[tag - 1];

		if (group->used && !group->anchored && !group->rebuilt)
		{
			b3Body_SetLinearVelocity(body, b3MulSV(kept, group->linear_velocity_before_step));
			b3Body_SetAngularVelocity(body, group->angular_velocity_before_step);
		}
	}
	else if (tag < 0)
	{
		const damage_projectile_t *projectile = &model->projectiles[-tag - 1];

		b3Body_SetLinearVelocity(body, b3MulSV(kept, projectile->linear_velocity_before_step));
		b3Body_SetAngularVelocity(body, projectile->angular_velocity_before_step);
	}
}

void damage_model_after_step(damage_model_t *model, int frame_index, float step_seconds)
{
	b3ContactEvents events = b3World_GetContactEvents(model->world);
	b3BodyId restored_bodies[RESTORED_BODIES_MAX];
	int restored_count = 0;
	int i;
	int manifold_index;
	int point_index;

	for (i = 0; i < events.hitCount; i++)
	{
		const b3ContactHitEvent *event = &events.hitEvents[i];
		int piece_a;
		int piece_b;
		float impulse = 0.0f;
		bool broke = false;
		b3ContactData contact;
		damage_hit_t *hit;

		if (!b3Shape_IsValid(event->shapeIdA) || !b3Shape_IsValid(event->shapeIdB) || !b3Contact_IsValid(event->contactId))
		{
			continue;
		}
		// (A shape that is no piece, the ground or a ball, has no number)
		piece_a = (int)(intptr_t)b3Shape_GetUserData(event->shapeIdA) - 1;
		piece_b = (int)(intptr_t)b3Shape_GetUserData(event->shapeIdB) - 1;
		if (piece_a < 0 && piece_b < 0)
		{
			continue;
		}
		contact = b3Contact_GetData(event->contactId);
		for (manifold_index = 0; manifold_index < contact.manifoldCount; manifold_index++)
		{
			const b3Manifold *manifold = &contact.manifolds[manifold_index];

			for (point_index = 0; point_index < manifold->pointCount; point_index++)
			{
				impulse += manifold->points[point_index].totalNormalImpulse;
			}
		}

		if (piece_a >= 0 && damage_piece(model, piece_a, impulse, frame_index))
		{
			broke = true;
		}
		if (piece_b >= 0 && damage_piece(model, piece_b, impulse, frame_index))
		{
			broke = true;
		}
		if (broke && restored_count + 2 <= RESTORED_BODIES_MAX)
		{
			restored_bodies[restored_count++] = b3Shape_GetBody(event->shapeIdA);
			restored_bodies[restored_count++] = b3Shape_GetBody(event->shapeIdB);
		}

		hit = &model->recent_hits[model->hit_count % DAMAGE_RECENT_HITS_MAX];
		hit->frame_index = frame_index;
		hit->point = event->point;
		hit->impulse = impulse;
		hit->broke_links = broke;
		model->hit_count++;
		if (impulse > model->largest_hit_impulse)
		{
			model->largest_hit_impulse = impulse;
		}
	}

	// (Only now: the events point at shapes the rebuilding destroys)
	rebuild_dirty_groups(model);
	for (i = 0; i < restored_count; i++)
	{
		keep_going(model, restored_bodies[i]);
	}

	// What still stands: who bears what, if the graph changed; then what
	// is overloaded gives way a little more. A link that breaks here
	// makes the pass due again, at the next step: a collapse goes on
	// frame by frame.
	if (model->support_dirty)
	{
		work_out_support(model, frame_index);
	}
	give_way(model, frame_index, step_seconds);
	rebuild_dirty_groups(model);
	remove_crushed(model);
}

static b3Transform piece_world_transform(const damage_model_t *model, int piece_index)
{
	const damage_piece_t *piece = &model->pieces[piece_index];
	b3Transform frame = b3Body_GetTransform(model->groups[piece->group].body);
	b3Transform result;

	result.p = b3Add(frame.p, b3RotateVector(frame.q, piece->local.p));
	result.q = b3MulQuat(frame.q, piece->local.q);
	return result;
}

// How much of a piece faces a direction, square meters: what a blast
// from there pushes on
static float exposed_area(const damage_piece_t *piece, b3Quat rotation, b3Vec3 direction)
{
	if (piece->shape == DAMAGE_PIECE_BOX)
	{
		float along_x = fabsf(b3Dot(direction, b3RotateVector(rotation, make_vector(1.0f, 0.0f, 0.0f))));
		float along_y = fabsf(b3Dot(direction, b3RotateVector(rotation, make_vector(0.0f, 1.0f, 0.0f))));
		float along_z = fabsf(b3Dot(direction, b3RotateVector(rotation, make_vector(0.0f, 0.0f, 1.0f))));

		return 4.0f * (along_x * piece->half_extents.y * piece->half_extents.z
			+ along_y * piece->half_extents.x * piece->half_extents.z
			+ along_z * piece->half_extents.x * piece->half_extents.y);
	}
	else
	{
		float along_axis = fabsf(b3Dot(direction, b3RotateVector(rotation, make_vector(1.0f, 0.0f, 0.0f))));
		float across_axis = sqrtf(1.0f - (along_axis < 1.0f ? along_axis * along_axis : 1.0f));

		return 4.0f * piece->radius * piece->half_length * across_axis + PI * piece->radius * piece->radius * along_axis;
	}
}

// How strong a blast still is at a distance (1 within its radius, down
// to 0 over its falloff), and which way it pushes there
static float blast_reach(const damage_model_t *model, b3Vec3 blast_position, b3Vec3 point, b3Vec3 *direction)
{
	b3Vec3 offset = b3Sub(point, blast_position);
	float distance = b3Length(offset);
	float beyond = distance - model->settings.blast_radius;

	*direction = distance > 1e-4f ? b3MulSV(1.0f / distance, offset) : make_vector(0.0f, 1.0f, 0.0f);
	if (beyond <= 0.0f)
	{
		return 1.0f;
	}
	if (beyond >= model->settings.blast_falloff)
	{
		return 0.0f;
	}
	return 1.0f - beyond / model->settings.blast_falloff;
}

void damage_model_blast(damage_model_t *model, b3Vec3 position, float strength, int frame_index)
{
	int i;

	// What each piece gets, as things stand; the damage of it to the
	// links; then, once the groups are made anew, the push on what is loose
	for (i = 0; i < model->piece_count; i++)
	{
		b3Transform transform;
		b3Vec3 direction;
		float reach;
		float impulse;

		blast_impulses[i] = b3Vec3_zero;
		if (model->pieces[i].group == DAMAGE_GONE)
		{
			continue;
		}
		transform = piece_world_transform(model, i);
		reach = blast_reach(model, position, transform.p, &direction);
		impulse = strength * reach * exposed_area(&model->pieces[i], transform.q, direction);

		blast_points[i] = transform.p;
		blast_impulses[i] = b3MulSV(impulse, direction);
		if (impulse > 0.0f)
		{
			damage_piece(model, i, impulse, frame_index);
		}
	}
	rebuild_dirty_groups(model);
	for (i = 0; i < model->piece_count; i++)
	{
		const damage_group_t *group;

		if (model->pieces[i].group == DAMAGE_GONE || b3Length(blast_impulses[i]) <= 0.0f)
		{
			continue;
		}
		group = &model->groups[model->pieces[i].group];
		if (!group->anchored)
		{
			b3Body_ApplyLinearImpulse(group->body, blast_impulses[i], blast_points[i], true);
		}
	}
	for (i = 0; i < model->projectile_count; i++)
	{
		const damage_projectile_t *projectile = &model->projectiles[i];
		b3Vec3 center = b3Body_GetPosition(projectile->body);
		b3Vec3 direction;
		float reach = blast_reach(model, position, center, &direction);

		if (reach > 0.0f)
		{
			b3Body_ApplyLinearImpulseToCenter(projectile->body, b3MulSV(strength * reach * PI * projectile->radius * projectile->radius, direction), true);
		}
	}
}

// ---------------------------------------------------------------------
// Looking at it

// A body's color when it sleeps: half way to gray
static uint32_t asleep_color(uint32_t color)
{
	uint32_t red = (((color >> 16) & 0xFF) + ((ASLEEP_COLOR >> 16) & 0xFF)) / 2;
	uint32_t green = (((color >> 8) & 0xFF) + ((ASLEEP_COLOR >> 8) & 0xFF)) / 2;
	uint32_t blue = ((color & 0xFF) + (ASLEEP_COLOR & 0xFF)) / 2;

	return (red << 16) | (green << 8) | blue;
}

void damage_model_draw_solids(const damage_model_t *model, soft_render_t *renderer, uint32_t first_object_id)
{
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		const damage_piece_t *piece = &model->pieces[i];
		const damage_group_t *group;
		b3Transform transform;
		uint32_t color;

		if (piece->group == DAMAGE_GONE)
		{
			continue;
		}
		group = &model->groups[piece->group];
		transform = piece_world_transform(model, i);
		// (What stands is shown in its own color; what lies still after
		// coming loose is grayed; what was crushed is dark for the frame
		// or two before it goes)
		color = (group->anchored || b3Body_IsAwake(group->body)) ? piece->color : asleep_color(piece->color);
		if (piece->crushed)
		{
			color = CRUSHED_COLOR;
		}

		soft_render_set_object(renderer, first_object_id + (uint32_t)i);
		if (piece->shape == DAMAGE_PIECE_BOX)
		{
			soft_render_box(renderer, vector_floats(&transform.p), rotation_floats(&transform.q), vector_floats(&piece->half_extents), color);
		}
		else
		{
			// The very prism its collision shape is
			b3Vec3 half_axis = b3RotateVector(transform.q, make_vector(piece->half_length, 0.0f, 0.0f));
			b3Vec3 end_a = b3Sub(transform.p, half_axis);
			b3Vec3 end_b = b3Add(transform.p, half_axis);
			b3Vec3 first_corner = b3RotateVector(transform.q, make_vector(0.0f, cosf(LOG_FIRST_CORNER_RADIANS), sinf(LOG_FIRST_CORNER_RADIANS)));

			soft_render_prism(renderer, vector_floats(&end_a), vector_floats(&end_b), vector_floats(&first_corner), piece->radius, LOG_SIDE_COUNT, color);
		}
	}
	for (i = 0; i < model->projectile_count; i++)
	{
		const damage_projectile_t *projectile = &model->projectiles[i];
		b3Vec3 center = b3Body_GetPosition(projectile->body);

		soft_render_set_object(renderer, first_object_id + (uint32_t)(model->piece_count + i));
		soft_render_sphere(renderer, vector_floats(&center), projectile->radius,
			b3Body_IsAwake(projectile->body) ? PROJECTILE_COLOR : asleep_color(PROJECTILE_COLOR));
	}
}

void damage_model_draw_marks(const damage_model_t *model, soft_render_t *renderer, int frame_index, bool draw_debug)
{
	int first_hit = model->hit_count > DAMAGE_RECENT_HITS_MAX ? model->hit_count - DAMAGE_RECENT_HITS_MAX : 0;
	int i;

	if (draw_debug)
	{
		// The links, through the solids: most are inside the building
		renderer->marks_on_top = true;
		for (i = 0; i < model->link_count; i++)
		{
			const damage_link_t *link = &model->links[i];
			b3Transform frame;
			b3Vec3 point;

			if (model->pieces[link->piece_a].group == DAMAGE_GONE)
			{
				continue;
			}
			frame = b3Body_GetTransform(model->groups[model->pieces[link->piece_a].group].body);
			point = b3Add(frame.p, b3RotateVector(frame.q, link->local_point));

			if (link->broken)
			{
				if (frame_index - link->broken_frame < BROKEN_SHOWN_FRAMES)
				{
					soft_render_point(renderer, vector_floats(&point), 5, LINK_BROKEN_COLOR);
				}
			}
			else
			{
				bool hurt = link->hit_points_left < model->settings.links[link->type].hit_points;

				if (link->overload > 1.0f)
				{
					soft_render_point(renderer, vector_floats(&point), 6, LINK_OVERLOADED_COLOR);
				}
				else
				{
					soft_render_point(renderer, vector_floats(&point), hurt ? 4 : 3, hurt ? LINK_HURT_COLOR : LINK_WHOLE_COLOR);
				}
			}
		}
		renderer->marks_on_top = false;
	}

	// The hits of the last frames; with debug, each one's impulse
	for (i = first_hit; i < model->hit_count; i++)
	{
		const damage_hit_t *hit = &model->recent_hits[i % DAMAGE_RECENT_HITS_MAX];
		float screen_x;
		float screen_y;

		if (hit->frame_index > frame_index || frame_index - hit->frame_index >= HIT_SHOWN_FRAMES)
		{
			continue;
		}
		soft_render_point(renderer, vector_floats(&hit->point), hit->broke_links ? 9 : 5, HIT_COLOR);
		if (draw_debug && soft_render_project(renderer, vector_floats(&hit->point), &screen_x, &screen_y))
		{
			char text[32];

			snprintf(text, sizeof(text), "%.0f NS", (double)hit->impulse);
			soft_render_text(renderer, (int)screen_x + 6, (int)screen_y - 4, 1, LABEL_COLOR, text);
		}
	}
}

void damage_model_describe(const damage_model_t *model, char *text, int text_size)
{
	int standing_count = 0;
	int loose_count = 0;
	int rubble_count = 0;
	int hurt_count = 0;
	int i;

	for (i = 0; i < DAMAGE_PIECES_MAX; i++)
	{
		const damage_group_t *group = &model->groups[i];

		if (!group->used)
		{
			continue;
		}
		if (group->anchored)
		{
			standing_count++;
		}
		else if (group->rubble)
		{
			rubble_count++;
		}
		else
		{
			loose_count++;
		}
	}
	for (i = 0; i < model->link_count; i++)
	{
		const damage_link_t *link = &model->links[i];

		if (!link->broken && link->hit_points_left < model->settings.links[link->type].hit_points)
		{
			hurt_count++;
		}
	}
	snprintf(text, (size_t)text_size, "%d PIECES: %d GROUPS STANDING, %d LOOSE, %d RUBBLE  %d LINKS: %d BROKEN, %d HURT, %d OVERLOADED  %d HITS, LARGEST %.0f NS",
		model->piece_count, standing_count, loose_count, rubble_count, model->link_count, model->broken_link_count, hurt_count,
		model->overloaded_count, model->hit_count, (double)model->largest_hit_impulse);
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	size_t k;

	for (k = 0; k < size; k++)
	{
		hash = (hash ^ bytes[k]) * 16777619u;
	}
	return hash;
}

// (FNV-1a over where every piece is and what every link has left)
uint32_t damage_model_checksum(const damage_model_t *model)
{
	uint32_t hash = 2166136261u;
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		b3Transform transform;
		float values[7];

		if (model->pieces[i].group == DAMAGE_GONE)
		{
			continue;
		}
		transform = b3Body_GetTransform(model->groups[model->pieces[i].group].body);
		values[0] = transform.p.x;
		values[1] = transform.p.y;
		values[2] = transform.p.z;
		values[3] = transform.q.v.x;
		values[4] = transform.q.v.y;
		values[5] = transform.q.v.z;
		values[6] = transform.q.s;
		hash = hash_bytes(hash, values, sizeof(values));
	}
	for (i = 0; i < model->link_count; i++)
	{
		hash = hash_bytes(hash, &model->links[i].hit_points_left, sizeof(float));
	}
	for (i = 0; i < model->projectile_count; i++)
	{
		b3Vec3 position = b3Body_GetPosition(model->projectiles[i].body);

		hash = hash_bytes(hash, &position, sizeof(position));
	}
	return hash;
}
