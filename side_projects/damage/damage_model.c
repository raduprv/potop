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
// A long piece's bending is looked at where its links are and at this
// many places evenly along it; and a piece's links looked at, at most
#define BEND_SAMPLES 16
#define BEND_LOADS_MAX 48
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
	link->slide = pull;
	link->cantilever = cantilever;
	link->grip = 0.0f;
	link->preload = 0.0f;
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
	settings->piece_threshold[DAMAGE_MATERIAL_WOOD] = 6000.0f;
	settings->piece_hit_points[DAMAGE_MATERIAL_WOOD] = 12000.0f;
	settings->bend[DAMAGE_MATERIAL_WOOD] = 300000.0f;
	settings->snap[DAMAGE_MATERIAL_WOOD] = 150000.0f;
	settings->piece_threshold[DAMAGE_MATERIAL_BRICK] = 5000.0f;
	settings->piece_hit_points[DAMAGE_MATERIAL_BRICK] = 6000.0f;
	settings->bend[DAMAGE_MATERIAL_BRICK] = 200000.0f;
	settings->snap[DAMAGE_MATERIAL_BRICK] = 60000.0f;
	settings->piece_threshold[DAMAGE_MATERIAL_GLASS] = 100.0f;
	settings->piece_hit_points[DAMAGE_MATERIAL_GLASS] = 300.0f;
	settings->bend[DAMAGE_MATERIAL_GLASS] = 20000.0f;
	settings->snap[DAMAGE_MATERIAL_GLASS] = 3000.0f;
	settings->long_ratio = 4.0f;
	settings->cut_min_length = 0.3f;
	settings->cut_clearance = 0.05f;
	set_link_defaults(&settings->links[DAMAGE_LINK_NAILS], 150.0f, 600.0f, 200000.0f, 4000.0f, 2500.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_MORTAR], 60.0f, 200.0f, 500000.0f, 3000.0f, 900.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_FOUNDATION], 300.0f, 1500.0f, 1000000.0f, 5000.0f, 5000.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_GLAZING], 30.0f, 60.0f, 100000.0f, 500.0f, 200.0f);
	set_link_defaults(&settings->links[DAMAGE_LINK_NOTCH], 60.0f, 400.0f, 200000.0f, 300.0f, 300.0f);
	settings->links[DAMAGE_LINK_NOTCH].slide = 2500.0f;
	settings->links[DAMAGE_LINK_NOTCH].grip = 0.6f;
	settings->links[DAMAGE_LINK_NOTCH].preload = 0.1f;
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
	settings->blast_strength = 3000.0f;
	settings->blast_big_strength = 6000.0f;
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
	if (strcmp(key, "slide") == 0)
	{
		return read_float(value, &link->slide);
	}
	if (strcmp(key, "cantilever") == 0)
	{
		return read_float(value, &link->cantilever);
	}
	if (strcmp(key, "grip") == 0)
	{
		return read_float(value, &link->grip);
	}
	if (strcmp(key, "preload") == 0)
	{
		return read_float(value, &link->preload);
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
	if (strcmp(key, "threshold") == 0)
	{
		return read_float(value, &settings->piece_threshold[material]);
	}
	if (strcmp(key, "hit_points") == 0)
	{
		return read_float(value, &settings->piece_hit_points[material]);
	}
	if (strcmp(key, "bend") == 0)
	{
		return read_float(value, &settings->bend[material]);
	}
	if (strcmp(key, "snap") == 0)
	{
		return read_float(value, &settings->snap[material]);
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
		if (strcmp(key, "long_ratio") == 0)
		{
			return read_float(value, &settings->long_ratio);
		}
		if (strcmp(key, "cut_clearance") == 0)
		{
			return read_float(value, &settings->cut_clearance);
		}
		if (strcmp(key, "cut_min_length") == 0)
		{
			return read_float(value, &settings->cut_min_length);
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
	if (strcmp(section, "blast") == 0)
	{
		if (strcmp(key, "strength") == 0)
		{
			return read_float(value, &settings->blast_strength);
		}
		if (strcmp(key, "big_strength") == 0)
		{
			return read_float(value, &settings->blast_big_strength);
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
	if (strcmp(section, "notch") == 0)
	{
		return read_link_key(&settings->links[DAMAGE_LINK_NOTCH], key, value);
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

// A piece's half sizes along its own x, y and z (a log is as thick and
// as wide as its radius)
static void piece_half_sizes(const damage_piece_t *piece, float half_sizes[3])
{
	if (piece->shape == DAMAGE_PIECE_BOX)
	{
		half_sizes[0] = piece->half_extents.x;
		half_sizes[1] = piece->half_extents.y;
		half_sizes[2] = piece->half_extents.z;
	}
	else
	{
		half_sizes[0] = piece->half_length;
		half_sizes[1] = piece->radius;
		half_sizes[2] = piece->radius;
	}
}

// What follows from a piece's sizes: its mass, and whether (and along
// which of its axes) it is long. Again after it is cut.
static void measure_piece(const damage_model_t *model, damage_piece_t *piece)
{
	float half_sizes[3];
	int longest = 0;
	float widest_across = 0.0f;
	int k;

	piece_half_sizes(piece, half_sizes);
	if (piece->shape == DAMAGE_PIECE_BOX)
	{
		piece->mass = 8.0f * half_sizes[0] * half_sizes[1] * half_sizes[2] * model->settings.densities[piece->material];
	}
	else
	{
		// (The prism's cross section: 12 triangles of two sides a radius,
		// 30 degrees between them, 3 radius squared in all)
		piece->mass = 3.0f * piece->radius * piece->radius * 2.0f * piece->half_length * model->settings.densities[piece->material];
	}
	for (k = 1; k < 3; k++)
	{
		if (half_sizes[k] > half_sizes[longest])
		{
			longest = k;
		}
	}
	for (k = 0; k < 3; k++)
	{
		if (k != longest && half_sizes[k] > widest_across)
		{
			widest_across = half_sizes[k];
		}
	}
	piece->long_axis = half_sizes[longest] >= model->settings.long_ratio * widest_across ? longest : -1;
}

// The bending moment a long piece bears, for each newton meter (or
// newton second meter) of its material's strength: its thickness squared
// times its width, across its length
static float piece_section(const damage_piece_t *piece)
{
	float half_sizes[3];
	float thickness = 1e30f;
	float width = 0.0f;
	int k;

	piece_half_sizes(piece, half_sizes);
	for (k = 0; k < 3; k++)
	{
		if (k == piece->long_axis)
		{
			continue;
		}
		if (2.0f * half_sizes[k] < thickness)
		{
			thickness = 2.0f * half_sizes[k];
		}
		if (2.0f * half_sizes[k] > width)
		{
			width = 2.0f * half_sizes[k];
		}
	}
	return thickness * thickness * width;
}

// A long piece's direction in the building's frame
static b3Vec3 piece_axis(const damage_piece_t *piece)
{
	b3Vec3 own = make_vector(piece->long_axis == 0 ? 1.0f : 0.0f, piece->long_axis == 1 ? 1.0f : 0.0f, piece->long_axis == 2 ? 1.0f : 0.0f);

	return b3RotateVector(piece->local.q, own);
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
	piece->hit_points_left = model->settings.piece_hit_points[material];
	piece->long_axis = -1;
	return model->piece_count++;
}

int damage_model_add_box(damage_model_t *model, b3Vec3 position, b3Quat rotation, b3Vec3 half_extents, damage_material_t material, uint32_t color)
{
	int piece_index = add_piece(model, DAMAGE_PIECE_BOX, position, rotation, material, color);

	if (piece_index >= 0)
	{
		model->pieces[piece_index].half_extents = half_extents;
		measure_piece(model, &model->pieces[piece_index]);
	}
	return piece_index;
}

int damage_model_add_log(damage_model_t *model, b3Vec3 position, b3Quat rotation, float radius, float half_length, damage_material_t material, uint32_t color)
{
	int piece_index = add_piece(model, DAMAGE_PIECE_LOG, position, rotation, material, color);

	if (piece_index >= 0)
	{
		model->pieces[piece_index].radius = radius;
		model->pieces[piece_index].half_length = half_length;
		measure_piece(model, &model->pieces[piece_index]);
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
	link->held_piece = -1;
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

// Every piece's links listed one after another (piece_link_start and
// piece_links): at the start, and again when a cut made a piece more
// and moved links to it
static void list_piece_links(damage_model_t *model)
{
	int fill[DAMAGE_PIECES_MAX];
	int i;

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
}

void damage_model_finish(damage_model_t *model)
{
	int i;

	if (model->finished)
	{
		return;
	}
	model->finished = true;
	model->support_dirty = true;

	list_piece_links(model);

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

	if (part_count == 1 && model->groups[group_parts[0]].anchored == old_group.anchored && !old_group.reshaped)
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

// A share of an impulse on a link. True when that broke it. (What
// presses on the link, as the last support pass found it, adds to what
// it shrugs off: preload.)
static bool hurt_link(damage_model_t *model, int link_index, float share, int frame_index)
{
	damage_link_t *link = &model->links[link_index];
	const damage_link_settings_t *limits = &model->settings.links[link->type];
	float threshold = limits->threshold + limits->preload * link->static_compression;

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

// A hit on the piece itself: over its material's threshold it costs it
// hit points, and with none left it is no more (all its links break; it
// is taken out of the world once it is a group of its own). True when
// that happened.
static bool hurt_piece(damage_model_t *model, int piece_index, float impulse, int frame_index)
{
	damage_piece_t *piece = &model->pieces[piece_index];
	float threshold = model->settings.piece_threshold[piece->material];
	int k;

	if (piece->group == DAMAGE_GONE || piece->crushed || impulse <= threshold)
	{
		return false;
	}
	piece->hit_points_left -= impulse - threshold;
	if (piece->hit_points_left > 0.0f)
	{
		return false;
	}
	piece->hit_points_left = 0.0f;
	piece->crushed = true;
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		break_link(model, model->piece_links[k], frame_index);
	}
	return true;
}

// Asks for a long piece to be cut at a place along it (meters from its
// middle), when the graph is no longer being walked. The place is moved
// in from the ends so no part is shorter than cut_min_length; a piece
// too short for two such parts is left whole, and so is one already
// waiting to be cut. Then it is moved off any piece linked to this one
// (by cut_clearance past its edge, where that leaves both parts long
// enough): a plank breaks at the edge of what holds it, not over it,
// where its loose part would be left lying on a sliver of its support.
// True when the cut will be made.
static bool request_cut(damage_model_t *model, int piece_index, float position)
{
	const damage_piece_t *piece = &model->pieces[piece_index];
	b3Vec3 axis;
	float half_sizes[3];
	float furthest;
	int i;
	int k;

	if (piece->long_axis < 0 || model->piece_count + model->pending_cut_count >= DAMAGE_PIECES_MAX)
	{
		return false;
	}
	piece_half_sizes(piece, half_sizes);
	furthest = half_sizes[piece->long_axis] - model->settings.cut_min_length;
	if (furthest < 0.0f)
	{
		return false;
	}
	for (i = 0; i < model->pending_cut_count; i++)
	{
		if (model->pending_cut_pieces[i] == piece_index)
		{
			return false;
		}
	}
	position = position > furthest ? furthest : (position < -furthest ? -furthest : position);
	axis = piece_axis(piece);
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		const damage_link_t *link = &model->links[model->piece_links[k]];
		const damage_piece_t *other;
		int other_index = link->piece_a == piece_index ? link->piece_b : link->piece_a;
		float other_half_sizes[3];
		float other_middle;
		float other_reach = 0.0f;
		float before;
		float after;
		int axis_index;

		if (link->broken || other_index < 0)
		{
			continue;
		}
		// Where along this piece the other one is, and how far it
		// reaches either way
		other = &model->pieces[other_index];
		piece_half_sizes(other, other_half_sizes);
		other_middle = b3Dot(b3Sub(other->local.p, piece->local.p), axis);
		for (axis_index = 0; axis_index < 3; axis_index++)
		{
			b3Vec3 unit = { axis_index == 0 ? 1.0f : 0.0f, axis_index == 1 ? 1.0f : 0.0f, axis_index == 2 ? 1.0f : 0.0f };

			other_reach += fabsf(b3Dot(b3RotateVector(other->local.q, unit), axis)) * other_half_sizes[axis_index];
		}
		before = other_middle - other_reach - model->settings.cut_clearance;
		after = other_middle + other_reach + model->settings.cut_clearance;
		if (position <= before || position >= after)
		{
			continue;
		}
		// To the nearer edge if both parts stay long enough, else to
		// the other, else it stays
		if (position - before > after - position)
		{
			float swap = before;

			before = after;
			after = swap;
		}
		if (before >= -furthest && before <= furthest)
		{
			position = before;
		}
		else if (after >= -furthest && after <= furthest)
		{
			position = after;
		}
	}
	model->pending_cut_pieces[model->pending_cut_count] = piece_index;
	model->pending_cut_positions[model->pending_cut_count] = position;
	model->pending_cut_count++;
	return true;
}

// Cuts a long piece in two, straight across, at a place along it: the
// piece keeps the part before the cut, a new piece is the part after it,
// and each link goes with the part it is on. (Nothing joins the two.)
static void cut_piece(damage_model_t *model, int piece_index, float position)
{
	damage_piece_t *piece = &model->pieces[piece_index];
	damage_piece_t *other_part = &model->pieces[model->piece_count];
	b3Vec3 axis = piece_axis(piece);
	b3Vec3 old_center = piece->local.p;
	float half_sizes[3];
	float half_length;
	float first_half;
	float second_half;
	int axis_index = piece->long_axis;
	int i;

	piece_half_sizes(piece, half_sizes);
	half_length = half_sizes[axis_index];
	first_half = (position + half_length) * 0.5f;
	second_half = (half_length - position) * 0.5f;

	*other_part = *piece;
	piece->local.p = b3Add(old_center, b3MulSV(-half_length + first_half, axis));
	other_part->local.p = b3Add(old_center, b3MulSV(position + second_half, axis));
	if (piece->shape == DAMAGE_PIECE_BOX)
	{
		float *first_extents = &piece->half_extents.x;
		float *second_extents = &other_part->half_extents.x;

		first_extents[axis_index] = first_half;
		second_extents[axis_index] = second_half;
	}
	else
	{
		piece->half_length = first_half;
		other_part->half_length = second_half;
	}
	measure_piece(model, piece);
	measure_piece(model, other_part);
	piece->bent = 0.0f;
	other_part->bent = 0.0f;
	memcpy(&model->support_solution[model->piece_count * 6], &model->support_solution[piece_index * 6], 6 * sizeof(double));

	for (i = 0; i < model->link_count; i++)
	{
		damage_link_t *link = &model->links[i];

		if ((link->piece_a == piece_index || link->piece_b == piece_index) && b3Dot(b3Sub(link->local_point, old_center), axis) > position)
		{
			if (link->piece_a == piece_index)
			{
				link->piece_a = model->piece_count;
			}
			else
			{
				link->piece_b = model->piece_count;
			}
		}
	}
	model->piece_count++;
	model->cut_count++;
	model->groups[piece->group].reshaped = true;
	model->group_dirty[piece->group] = true;
	model->any_group_dirty = true;
	model->support_dirty = true;
}

// The cuts asked for are made, and every piece's links listed again
static void make_pending_cuts(damage_model_t *model)
{
	int i;

	if (model->pending_cut_count == 0)
	{
		return;
	}
	for (i = 0; i < model->pending_cut_count; i++)
	{
		if (model->pieces[model->pending_cut_pieces[i]].group != DAMAGE_GONE && model->piece_count < DAMAGE_PIECES_MAX)
		{
			cut_piece(model, model->pending_cut_pieces[i], model->pending_cut_positions[i]);
		}
	}
	model->pending_cut_count = 0;
	list_piece_links(model);
}

// A hit on a long piece bends it between the links nearest the hit on
// either side that hold it, as a plank across two trestles struck
// between them (a hit past the last such link: as a plank struck on its
// free end; a piece no link holds: half as much, its own weight is all
// that resists). Only
// the part of the impulse across the piece counts. Where that bending
// is more than the piece's material and thickness bear, it is cut at
// the hit. True when it will be.
static bool snap_piece(damage_model_t *model, int piece_index, b3Vec3 world_point, b3Vec3 world_direction, float impulse)
{
	const damage_piece_t *piece = &model->pieces[piece_index];
	b3Transform frame;
	b3Vec3 axis;
	b3Vec3 direction;
	float half_sizes[3];
	float half_length;
	float position;
	float along;
	float across_impulse;
	float before = -1.0f;
	float after = -1.0f;
	float bending;
	int k;

	if (piece->group == DAMAGE_GONE || piece->long_axis < 0)
	{
		return false;
	}
	frame = b3Body_GetTransform(model->groups[piece->group].body);
	axis = piece_axis(piece);
	direction = b3InvRotateVector(frame.q, world_direction);
	position = b3Dot(b3Sub(b3InvRotateVector(frame.q, b3Sub(world_point, frame.p)), piece->local.p), axis);
	along = b3Dot(direction, axis);
	across_impulse = impulse * sqrtf(1.0f - (along * along < 1.0f ? along * along : 1.0f));
	piece_half_sizes(piece, half_sizes);
	half_length = half_sizes[piece->long_axis];

	// How far the nearest whole link that holds the piece is on each side
	// of the hit (not a link by which something lies on it or hangs from
	// it: that does not stop it bending; for a piece that does not stand,
	// any link)
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		const damage_link_t *link = &model->links[model->piece_links[k]];
		float distance;

		if (link->broken || (link->held_piece >= 0 && link->held_piece != piece_index))
		{
			continue;
		}
		distance = b3Dot(b3Sub(link->local_point, piece->local.p), axis) - position;
		if (distance < 0.0f && (before < 0.0f || -distance < before))
		{
			before = -distance;
		}
		if (distance >= 0.0f && (after < 0.0f || distance < after))
		{
			after = distance;
		}
	}
	if (before >= 0.0f && after >= 0.0f)
	{
		bending = before + after > 1e-4f ? across_impulse * before * after / (before + after) : 0.0f;
	}
	else if (before >= 0.0f || after >= 0.0f)
	{
		bending = across_impulse * (before >= 0.0f ? before : after);
	}
	else
	{
		float to_start = position + half_length;
		float to_end = half_length - position;

		bending = to_start > 0.0f && to_end > 0.0f ? 0.5f * across_impulse * to_start * to_end / (to_start + to_end) : 0.0f;
	}
	if (bending <= model->settings.snap[piece->material] * piece_section(piece))
	{
		return false;
	}
	return request_cut(model, piece_index, position);
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
		model->links[i].static_pull = 0.0f;
		model->links[i].static_sideways = 0.0f;
		model->links[i].static_moment = 0.0f;
		model->links[i].held_piece = -1;
	}
	for (i = 0; i < model->piece_count; i++)
	{
		model->pieces[i].pressed = 0.0f;
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

		// (Pressed, it bears more from the side by its grip; so with
		// turning, as much as that grip would hold at joint_arm)
		overload = compression / limits->compression;
		if (pull / limits->pull > overload)
		{
			overload = pull / limits->pull;
		}
		if (sideways / (limits->slide + limits->grip * compression) > overload)
		{
			overload = sideways / (limits->slide + limits->grip * compression);
		}
		if (moment / (limits->cantilever + limits->grip * compression * model->settings.joint_arm) > overload)
		{
			overload = moment / (limits->cantilever + limits->grip * compression * model->settings.joint_arm);
		}
		link->overload = overload;
		link->static_compression = compression;
		link->static_pull = pull;
		link->static_sideways = sideways;
		link->static_moment = moment;
		// (The spring pulls piece a back the way it came apart: downwards
		// apart means it holds a up, else b)
		link->held_piece = apart[1] < 0.0 ? link->piece_a : link->piece_b;
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

	// The long pieces: what bends each one. Its links' forces on it and its
	// own weight, taken up to a place along it, turn about that place:
	// the part of that turning across the piece is the bending there.
	// Where it is most, and more than the piece bears, it is cut.
	for (i = 0; i < queue_end; i++)
	{
		int piece_index = walk_queue[i];
		damage_piece_t *piece = &model->pieces[piece_index];
		double load_positions[BEND_LOADS_MAX];
		double load_forces[BEND_LOADS_MAX][3];
		double load_moments[BEND_LOADS_MAX][3];
		double axis[3];
		float half_sizes[3];
		double half_length;
		double weight;
		double limit;
		double worst_bending = 0.0;
		double worst_position = 0.0;
		int load_count = 0;
		int sample;

		piece->bent = 0.0f;
		if (piece->long_axis < 0)
		{
			continue;
		}
		{
			b3Vec3 direction = piece_axis(piece);

			axis[0] = (double)direction.x;
			axis[1] = (double)direction.y;
			axis[2] = (double)direction.z;
		}
		piece_half_sizes(piece, half_sizes);
		half_length = (double)half_sizes[piece->long_axis];
		weight = (double)piece->mass * (double)GRAVITY;
		limit = (double)model->settings.bend[piece->material] * (double)piece_section(piece);

		for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1] && load_count < BEND_LOADS_MAX; k++)
		{
			const damage_link_t *link = &model->links[model->piece_links[k]];
			double apart[3];
			double turned[3];
			// (The spring pulls piece a back the way it came apart, and
			// piece b the other way)
			double sign = link->piece_a == piece_index ? -1.0 : 1.0;
			double arm[3];

			if (link->broken)
			{
				continue;
			}
			link_strain(model, link, solver_x, apart, turned);
			arm[0] = (double)link->local_point.x - (double)piece->local.p.x;
			arm[1] = (double)link->local_point.y - (double)piece->local.p.y;
			arm[2] = (double)link->local_point.z - (double)piece->local.p.z;
			load_positions[load_count] = arm[0] * axis[0] + arm[1] * axis[1] + arm[2] * axis[2];
			// The force, and its turning about the piece's MIDDLE (moved to
			// the place looked at below) with the link's own turning
			load_forces[load_count][0] = sign * apart[0];
			load_forces[load_count][1] = sign * apart[1];
			load_forces[load_count][2] = sign * apart[2];
			load_moments[load_count][0] = arm[1] * load_forces[load_count][2] - arm[2] * load_forces[load_count][1] + sign * turn_stiffness * turned[0];
			load_moments[load_count][1] = arm[2] * load_forces[load_count][0] - arm[0] * load_forces[load_count][2] + sign * turn_stiffness * turned[1];
			load_moments[load_count][2] = arm[0] * load_forces[load_count][1] - arm[1] * load_forces[load_count][0] + sign * turn_stiffness * turned[2];
			load_count++;
		}

		for (sample = 0; sample < BEND_SAMPLES + load_count; sample++)
		{
			// Evenly along it, then just before each link
			double position = sample < BEND_SAMPLES ? -half_length + 2.0 * half_length * ((double)sample + 0.5) / (double)BEND_SAMPLES : load_positions[sample - BEND_SAMPLES];
			double force[3] = {0.0, 0.0, 0.0};
			double moment[3] = {0.0, 0.0, 0.0};
			double part_weight;
			double part_middle;
			double turning[3];
			double along;
			double bending;
			int load;

			if (position <= -half_length || position >= half_length)
			{
				continue;
			}
			// All that acts on the part before this place: the links there...
			for (load = 0; load < load_count; load++)
			{
				if (load_positions[load] < position)
				{
					force[0] += load_forces[load][0];
					force[1] += load_forces[load][1];
					force[2] += load_forces[load][2];
					moment[0] += load_moments[load][0];
					moment[1] += load_moments[load][1];
					moment[2] += load_moments[load][2];
				}
			}
			// ...and that part's own weight, at its own middle
			part_weight = weight * (position + half_length) / (2.0 * half_length);
			part_middle = (position - half_length) * 0.5;
			force[1] -= part_weight;
			moment[0] += part_middle * axis[2] * part_weight;
			moment[2] -= part_middle * axis[0] * part_weight;
			// Its turning about the place itself: about the middle, less
			// the place's arm crossed with the force
			turning[0] = moment[0] - position * (axis[1] * force[2] - axis[2] * force[1]);
			turning[1] = moment[1] - position * (axis[2] * force[0] - axis[0] * force[2]);
			turning[2] = moment[2] - position * (axis[0] * force[1] - axis[1] * force[0]);
			// (Turning about the piece's own length twists it, it does not
			// bend it)
			along = turning[0] * axis[0] + turning[1] * axis[1] + turning[2] * axis[2];
			turning[0] -= along * axis[0];
			turning[1] -= along * axis[1];
			turning[2] -= along * axis[2];
			bending = sqrt(turning[0] * turning[0] + turning[1] * turning[1] + turning[2] * turning[2]);
			if (bending > worst_bending)
			{
				worst_bending = bending;
				worst_position = position;
			}
		}
		piece->bent = (float)(worst_bending / limit);
		if (worst_bending > limit)
		{
			request_cut(model, piece_index, (float)worst_position);
		}
	}

	// A piece squeezed between what lies on it and what it lies on is
	// pressed from both sides: half of all that presses on it is what
	// goes through it
	for (i = 0; i < queue_end; i++)
	{
		int piece_index = walk_queue[i];

		model->pieces[piece_index].pressed = piece_compressions[piece_index] * 0.5f;
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

		// Each piece hit: the hit on the piece itself, on its length if it
		// is long, and on its links
		if (piece_a >= 0)
		{
			if (hurt_piece(model, piece_a, impulse, frame_index))
			{
				broke = true;
			}
			if (snap_piece(model, piece_a, event->point, event->normal, impulse))
			{
				broke = true;
			}
			if (damage_piece(model, piece_a, impulse, frame_index))
			{
				broke = true;
			}
		}
		if (piece_b >= 0)
		{
			if (hurt_piece(model, piece_b, impulse, frame_index))
			{
				broke = true;
			}
			if (snap_piece(model, piece_b, event->point, event->normal, impulse))
			{
				broke = true;
			}
			if (damage_piece(model, piece_b, impulse, frame_index))
			{
				broke = true;
			}
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

	// (Only now: the events point at shapes the rebuilding destroys, and a
	// cut changes whose links are whose)
	make_pending_cuts(model);
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
	make_pending_cuts(model);
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
	int piece_count_before = model->piece_count;
	int i;

	// What each piece gets, as things stand, and the damage of it: to the
	// piece, to its links, and a long piece may break across (pushed
	// where it is nearest the blast)
	for (i = 0; i < piece_count_before; i++)
	{
		const damage_piece_t *piece = &model->pieces[i];
		b3Transform transform;
		b3Vec3 direction;
		b3Vec3 nearest;
		float reach;
		float impulse;

		if (piece->group == DAMAGE_GONE)
		{
			continue;
		}
		transform = piece_world_transform(model, i);
		reach = blast_reach(model, position, transform.p, &direction);
		impulse = strength * reach * exposed_area(piece, transform.q, direction);
		if (impulse <= 0.0f)
		{
			continue;
		}
		nearest = transform.p;
		if (piece->long_axis >= 0)
		{
			b3Vec3 own_axis = { piece->long_axis == 0 ? 1.0f : 0.0f, piece->long_axis == 1 ? 1.0f : 0.0f, piece->long_axis == 2 ? 1.0f : 0.0f };
			b3Vec3 world_axis = b3RotateVector(transform.q, own_axis);
			float half_sizes[3];
			float along = b3Dot(b3Sub(position, transform.p), world_axis);

			piece_half_sizes(piece, half_sizes);
			along = along > half_sizes[piece->long_axis] ? half_sizes[piece->long_axis] : (along < -half_sizes[piece->long_axis] ? -half_sizes[piece->long_axis] : along);
			nearest = b3Add(transform.p, b3MulSV(along, world_axis));
		}
		if (!hurt_piece(model, i, impulse, frame_index))
		{
			snap_piece(model, i, nearest, direction, impulse);
		}
		damage_piece(model, i, impulse, frame_index);
	}
	make_pending_cuts(model);
	rebuild_dirty_groups(model);

	// Then the push on what is loose now (worked out again: pieces were
	// cut)
	for (i = 0; i < model->piece_count; i++)
	{
		const damage_piece_t *piece = &model->pieces[i];
		const damage_group_t *group;
		b3Transform transform;
		b3Vec3 direction;
		float reach;
		float impulse;

		if (piece->group == DAMAGE_GONE)
		{
			continue;
		}
		group = &model->groups[piece->group];
		if (group->anchored)
		{
			continue;
		}
		transform = piece_world_transform(model, i);
		reach = blast_reach(model, position, transform.p, &direction);
		impulse = strength * reach * exposed_area(piece, transform.q, direction);
		if (impulse > 0.0f)
		{
			b3Body_ApplyLinearImpulse(group->body, b3MulSV(impulse, direction), transform.p, true);
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

int damage_model_shape_piece(b3ShapeId shape)
{
	if (!b3Shape_IsValid(shape))
	{
		return -1;
	}
	return (int)(intptr_t)b3Shape_GetUserData(shape) - 1;
}

void damage_model_draw_inspection(const damage_model_t *model, soft_render_t *renderer, int piece_index, int text_x, int text_y, int text_scale)
{
	static const char *const material_names[DAMAGE_MATERIAL_COUNT] = {"WOOD", "BRICK", "GLASS"};
	static const char *const link_names[DAMAGE_LINK_TYPE_COUNT] = {"NAILS", "MORTAR", "FOUNDATION", "GLAZING", "NOTCH"};
	const damage_piece_t *piece;
	const damage_group_t *group;
	b3Transform frame;
	b3Vec3 center;
	char text[160];
	int line_height = (7 + 2) * text_scale;
	int whole_count = 0;
	int broken_count = 0;
	int k;

	if (piece_index < 0 || piece_index >= model->piece_count)
	{
		return;
	}
	piece = &model->pieces[piece_index];
	if (piece->group == DAMAGE_GONE)
	{
		snprintf(text, sizeof(text), "PIECE %d: %s, GONE", piece_index, material_names[piece->material]);
		soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
		return;
	}
	group = &model->groups[piece->group];
	frame = b3Body_GetTransform(group->body);
	center = b3Add(frame.p, b3RotateVector(frame.q, piece->local.p));

	// Its links, each as a line from its middle to the link and on to the
	// middle of what the link holds it to (straight down for the ground)
	renderer->marks_on_top = true;
	renderer->line_width_pixels = 2;
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		const damage_link_t *link = &model->links[model->piece_links[k]];
		b3Vec3 point = b3Add(frame.p, b3RotateVector(frame.q, link->local_point));
		b3Vec3 other_end;
		uint32_t color = link->overload > 1.0f ? LINK_OVERLOADED_COLOR : (link->overload > 0.5f ? LINK_HURT_COLOR : LINK_WHOLE_COLOR);
		int other;

		if (link->broken)
		{
			broken_count++;
			continue;
		}
		whole_count++;
		other = link->piece_a == piece_index ? link->piece_b : link->piece_a;
		if (other == DAMAGE_GROUND)
		{
			other_end = b3Sub(point, make_vector(0.0f, 0.15f, 0.0f));
		}
		else
		{
			other_end = b3Add(frame.p, b3RotateVector(frame.q, model->pieces[other].local.p));
		}
		soft_render_line(renderer, vector_floats(&center), vector_floats(&point), color);
		soft_render_line(renderer, vector_floats(&point), vector_floats(&other_end), color);
		soft_render_point(renderer, vector_floats(&point), 6, color);
	}
	soft_render_point(renderer, vector_floats(&center), 8, LABEL_COLOR);
	renderer->line_width_pixels = 1;
	renderer->marks_on_top = false;

	snprintf(text, sizeof(text), "PIECE %d: %s, %.0f KG, %s (GROUP OF %d)", piece_index, material_names[piece->material], (double)piece->mass,
		group->anchored ? "STANDS" : (group->rubble ? "RUBBLE" : "LOOSE"), group->piece_count);
	soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
	text_y += line_height;
	snprintf(text, sizeof(text), "PRESSED %.0f N OF %.0f (%.0f%%)   LINKS: %d WHOLE, %d BROKEN", (double)piece->pressed, (double)model->settings.crush[piece->material],
		(double)(100.0f * piece->pressed / model->settings.crush[piece->material]), whole_count, broken_count);
	soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
	text_y += line_height;
	if (piece->long_axis >= 0)
	{
		snprintf(text, sizeof(text), "HIT POINTS %.0f/%.0f   LONG: BENT %.0f%% OF WHAT IT BEARS", (double)piece->hit_points_left,
			(double)model->settings.piece_hit_points[piece->material], (double)(100.0f * piece->bent));
	}
	else
	{
		snprintf(text, sizeof(text), "HIT POINTS %.0f/%.0f", (double)piece->hit_points_left, (double)model->settings.piece_hit_points[piece->material]);
	}
	soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
	text_y += line_height;

	// (What a link bears is known only for what stands: for a loose piece
	// the last pass left zeros)
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		const damage_link_t *link = &model->links[model->piece_links[k]];
		int other = link->piece_a == piece_index ? link->piece_b : link->piece_a;
		char other_name[16];

		if (link->broken)
		{
			continue;
		}
		if (other == DAMAGE_GROUND)
		{
			snprintf(other_name, sizeof(other_name), "GROUND");
		}
		else
		{
			snprintf(other_name, sizeof(other_name), "%d", other);
		}
		snprintf(text, sizeof(text), "%s TO %s: PRESS %.0f  PULL %.0f  SIDE %.0f N  TURN %.0f NM  %.0f%% OF LIMIT  HP %.0f/%.0f",
			link_names[link->type], other_name, (double)link->static_compression, (double)link->static_pull, (double)link->static_sideways,
			(double)link->static_moment, (double)(100.0f * link->overload), (double)link->hit_points_left, (double)model->settings.links[link->type].hit_points);
		soft_render_text(renderer, text_x, text_y, text_scale, link->overload > 1.0f ? LINK_OVERLOADED_COLOR : LABEL_COLOR, text);
		text_y += line_height;
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
	snprintf(text, (size_t)text_size, "%d PIECES (%d CUTS): %d GROUPS STANDING, %d LOOSE, %d RUBBLE  %d LINKS: %d BROKEN, %d HURT, %d OVERLOADED  %d HITS, LARGEST %.0f NS",
		model->piece_count, model->cut_count, standing_count, loose_count, rubble_count, model->link_count, model->broken_link_count, hurt_count,
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
