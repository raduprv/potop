// The wreck model: see wreck.h

#include "wreck.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


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
// A hit's share smaller than this is not passed on from piece to piece
// (newton seconds)
#define HIT_SHARE_MIN 1.0f
// A long piece is taken in this many parts along its length when a
// blast's push on it is worked out
#define BLAST_PARTS 5
#define GAS_RAYS 192
// A ray that meets nothing this far out has got out (meters)
#define GAS_RAY_METERS 30.0f
#define GAS_RAY_OUT -1
#define GAS_RAY_GROUND -2
#define GAS_RAY_WALL -3
// A ray looks through this many things that can move at most, and goes
// on this far past where it met one (meters)
#define RAY_PASSES 8
#define RAY_STEP_METERS 0.05f
// A ray goes on through this many loose pieces at most
#define GAS_RAY_PASSES 6
// Rays within this of one another (the cosine of the angle between
// them: about 25 degrees) are neighbors
#define GAS_NEIGHBOR_COSINE 0.9f
// The air is not reckoned with for what moves slower than this (meters
// a second)
#define AIR_DRAG_SPEED_MIN 2.0f
// A loose piece is taken in this many parts along its longest side and
// this many along the next for what the air and the water do to it
#define FLUID_PARTS_LONG 4
#define FLUID_PARTS_WIDE 2
// Water slower than this does not wake what sleeps in it (meters a
// second)
#define WATER_WAKE_SPEED 0.03f
// ...nor does water that has risen or fallen along it, since it fell
// asleep, by less than what bears this much of its weight
#define WATER_WAKE_LIFT 0.1f
// A ball is pushed back by the air and the water this many times as
// hard as a flat face as wide (the air goes around it more easily)
#define BALL_DRAG_SHARE 0.4f
// Who bears what is worked out anew when the water's push on a standing
// piece has changed by this much of the piece's weight
#define WATER_LOAD_CHANGE 0.05f
// A loose long piece that was hit is looked at in this many slices along
// it, with at most this many of its body's contacts
#define BEND_SLICES 32
#define BEND_CONTACTS_MAX 32
#define SUPPORT_ROUNDS_MAX 4000
// A long piece's bending is looked at where its links are and at this
// many places evenly along it; and a piece's links looked at, at most
#define BEND_SAMPLES 16
#define BEND_LOADS_MAX 48


// Scratch for walking the graph (one walk at a time: nothing here is
// used from two threads)
static int walk_queue[WRECK_PIECES_MAX];
static float walk_amounts[WRECK_PIECES_MAX];
static int walk_piece_stamps[WRECK_PIECES_MAX];
static int walk_link_stamps[WRECK_LINKS_MAX];
static int walk_stamp;
static int group_members[WRECK_PIECES_MAX];
static int group_parts[WRECK_PIECES_MAX];
// The support pass's unknowns: six a standing piece (how far it moved
// along x, y, z, how far it turned about them), -1 for one that does not
// stand; and the vectors of its solver
static int support_unknowns[WRECK_PIECES_MAX];
static double solver_x[WRECK_PIECES_MAX * 6];
static double solver_load[WRECK_PIECES_MAX * 6];
static double solver_residual[WRECK_PIECES_MAX * 6];
static double solver_scaled[WRECK_PIECES_MAX * 6];
static double solver_direction[WRECK_PIECES_MAX * 6];
static double solver_product[WRECK_PIECES_MAX * 6];
static double solver_diagonal[WRECK_PIECES_MAX * 6];
static float piece_compressions[WRECK_PIECES_MAX];
static b3Vec3 blast_impulses[WRECK_PIECES_MAX];
static bool blast_was_standing[WRECK_PIECES_MAX];
static float blast_shares[WRECK_PIECES_MAX];
static b3Vec3 gas_impulses[WRECK_PIECES_MAX];
static b3Vec3 gas_turnings[WRECK_PIECES_MAX];
static b3Vec3 blast_turnings[WRECK_PIECES_MAX];
static float gas_factors[WRECK_PIECES_MAX];
static int gas_island_sizes[WRECK_PIECES_MAX];
// The rays that look for a blast's way out: which way each goes, and the
// piece it ends on (GAS_RAY_OUT: it got out; GAS_RAY_GROUND; GAS_RAY_WALL:
// on another model's piece)
static b3Vec3 gas_ray_directions[GAS_RAYS];
static int gas_ray_pieces[GAS_RAYS];
// How stiffly each unknown is held where it is by nothing but its
// piece's own mass: nothing when weights are solved for (they have all
// the time in the world), something when a blast is
static double solver_inertia[WRECK_PIECES_MAX * 6];

// The first projectile of the model that is this body, -1 if none is
static int find_projectile(const wreck_model_t *model, b3BodyId body)
{
	int i;

	for (i = 0; i < model->projectile_count; i++)
	{
		if (B3_ID_EQUALS(model->projectiles[i].body, body))
		{
			return i;
		}
	}
	return -1;
}

static b3Vec3 make_vector(float x, float y, float z)
{
	b3Vec3 vector = {x, y, z};

	return vector;
}

// ---------------------------------------------------------------------
// Settings

static void set_link_defaults(wreck_link_settings_t *link, float compression, float pull, float cantilever, float give)
{
	link->compression = compression;
	link->pull = pull;
	link->slide = pull;
	link->cantilever = cantilever;
	link->give = give;
	link->grip = 0.0f;
}

void wreck_settings_default(wreck_settings_t *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->densities[WRECK_MATERIAL_WOOD] = 600.0f;
	settings->densities[WRECK_MATERIAL_BRICK] = 1800.0f;
	settings->densities[WRECK_MATERIAL_GLASS] = 2500.0f;
	settings->crush[WRECK_MATERIAL_WOOD] = 300000.0f;
	settings->crush[WRECK_MATERIAL_BRICK] = 500000.0f;
	settings->crush[WRECK_MATERIAL_GLASS] = 9000.0f;
	settings->piece_threshold[WRECK_MATERIAL_WOOD] = 6000.0f;
	settings->piece_hit_points[WRECK_MATERIAL_WOOD] = 12000.0f;
	settings->bend[WRECK_MATERIAL_WOOD] = 3000000.0f;
	settings->snap[WRECK_MATERIAL_WOOD] = 150000.0f;
	settings->stretch[WRECK_MATERIAL_WOOD] = 0.01f;
	settings->stretch[WRECK_MATERIAL_BRICK] = 0.0003f;
	settings->stretch[WRECK_MATERIAL_GLASS] = 0.001f;
	settings->piece_threshold[WRECK_MATERIAL_BRICK] = 5000.0f;
	settings->piece_hit_points[WRECK_MATERIAL_BRICK] = 6000.0f;
	settings->bend[WRECK_MATERIAL_BRICK] = 200000.0f;
	settings->snap[WRECK_MATERIAL_BRICK] = 60000.0f;
	settings->piece_threshold[WRECK_MATERIAL_GLASS] = 100.0f;
	settings->piece_hit_points[WRECK_MATERIAL_GLASS] = 300.0f;
	settings->bend[WRECK_MATERIAL_GLASS] = 20000.0f;
	settings->snap[WRECK_MATERIAL_GLASS] = 3000.0f;
	settings->long_ratio = 4.0f;
	settings->cut_min_length = 0.3f;
	settings->cut_clearance = 0.05f;
	set_link_defaults(&settings->links[WRECK_LINK_NAILS], 200000.0f, 4000.0f, 2500.0f, 0.02f);
	settings->links[WRECK_LINK_NAILS].pull = 1000.0f;
	set_link_defaults(&settings->links[WRECK_LINK_MORTAR], 500000.0f, 3000.0f, 900.0f, 0.002f);
	set_link_defaults(&settings->links[WRECK_LINK_FOUNDATION], 1000000.0f, 5000.0f, 5000.0f, 0.01f);
	set_link_defaults(&settings->links[WRECK_LINK_GLAZING], 100000.0f, 500.0f, 200.0f, 0.002f);
	set_link_defaults(&settings->links[WRECK_LINK_NOTCH], 200000.0f, 300.0f, 300.0f, 0.05f);
	settings->links[WRECK_LINK_NOTCH].slide = 2500.0f;
	settings->links[WRECK_LINK_NOTCH].grip = 0.6f;
	settings->link_elastic = 0.1f;
	settings->hop_falloff = 0.5f;
	settings->hit_speed = 1.0f;
	settings->rubble_pieces = 3;
	settings->breakthrough_speed_kept = 0.85f;
	settings->blast_impulse_per_kilogram = 350.0f;
	settings->blast_falloff_power = 1.0f;
	settings->blast_charge_density = 1600.0f;
	settings->blast_shielding = 0.8f;
	settings->blast_inertia = 0.005f;
	settings->blast_slices = 8;
	settings->blast_gas = 0.5f;
	settings->blast_gas_venting = 3.0f;
	settings->blast_gas_rush = 1.0f;
	settings->air_drag = 0.7f;
	settings->air_density = 1.2f;
	settings->water_density = 1000.0f;
	settings->collapse_seconds = 1.5f;
	settings->joint_arm = 0.15f;
	settings->shape_gap = 0.003f;
	settings->ball_radius = 0.2f;
	settings->ball_density = 1000.0f;
	settings->blast_kilograms = 3.0f;
	settings->blast_big_kilograms = 9.0f;
}

static bool read_float(const char *value, float *result)
{
	return sscanf(value, "%f", result) == 1;
}

static bool read_link_key(wreck_link_settings_t *link, const char *key, const char *value)
{
	if (strcmp(key, "give") == 0)
	{
		return read_float(value, &link->give);
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
	return false;
}

static bool read_material_key(wreck_settings_t *settings, wreck_material_t material, const char *key, const char *value)
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
	if (strcmp(key, "stretch") == 0)
	{
		return read_float(value, &settings->stretch[material]);
	}
	return false;
}

bool wreck_settings_set(void *user, const char *section, const char *key, const char *value)
{
	wreck_settings_t *settings = user;

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
		if (strcmp(key, "blast_impulse_per_kilogram") == 0)
		{
			return read_float(value, &settings->blast_impulse_per_kilogram);
		}
		if (strcmp(key, "air_density") == 0)
		{
			return read_float(value, &settings->air_density);
		}
		if (strcmp(key, "water_density") == 0)
		{
			return read_float(value, &settings->water_density);
		}
		if (strcmp(key, "air_drag") == 0)
		{
			return read_float(value, &settings->air_drag);
		}
		if (strcmp(key, "blast_gas") == 0)
		{
			return read_float(value, &settings->blast_gas);
		}
		if (strcmp(key, "blast_gas_venting") == 0)
		{
			return read_float(value, &settings->blast_gas_venting);
		}
		if (strcmp(key, "blast_gas_rush") == 0)
		{
			return read_float(value, &settings->blast_gas_rush);
		}
		if (strcmp(key, "blast_slices") == 0)
		{
			return sscanf(value, "%d", &settings->blast_slices) == 1;
		}
		if (strcmp(key, "blast_charge_density") == 0)
		{
			return read_float(value, &settings->blast_charge_density);
		}
		if (strcmp(key, "link_elastic") == 0)
		{
			return read_float(value, &settings->link_elastic);
		}
		if (strcmp(key, "blast_inertia") == 0)
		{
			return read_float(value, &settings->blast_inertia);
		}
		if (strcmp(key, "blast_shielding") == 0)
		{
			return read_float(value, &settings->blast_shielding);
		}
		if (strcmp(key, "blast_falloff_power") == 0)
		{
			return read_float(value, &settings->blast_falloff_power);
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
		if (strcmp(key, "kilograms") == 0)
		{
			return read_float(value, &settings->blast_kilograms);
		}
		if (strcmp(key, "big_kilograms") == 0)
		{
			return read_float(value, &settings->blast_big_kilograms);
		}
		return false;
	}
	if (strcmp(section, "wood") == 0)
	{
		return read_material_key(settings, WRECK_MATERIAL_WOOD, key, value);
	}
	if (strcmp(section, "brick") == 0)
	{
		return read_material_key(settings, WRECK_MATERIAL_BRICK, key, value);
	}
	if (strcmp(section, "glass") == 0)
	{
		return read_material_key(settings, WRECK_MATERIAL_GLASS, key, value);
	}
	if (strcmp(section, "nails") == 0)
	{
		return read_link_key(&settings->links[WRECK_LINK_NAILS], key, value);
	}
	if (strcmp(section, "mortar") == 0)
	{
		return read_link_key(&settings->links[WRECK_LINK_MORTAR], key, value);
	}
	if (strcmp(section, "foundation") == 0)
	{
		return read_link_key(&settings->links[WRECK_LINK_FOUNDATION], key, value);
	}
	if (strcmp(section, "glazing") == 0)
	{
		return read_link_key(&settings->links[WRECK_LINK_GLAZING], key, value);
	}
	if (strcmp(section, "notch") == 0)
	{
		return read_link_key(&settings->links[WRECK_LINK_NOTCH], key, value);
	}
	return false;
}

// ---------------------------------------------------------------------
// Building

void wreck_model_create(wreck_model_t *model, b3WorldId world, const wreck_settings_t *settings)
{
	memset(model, 0, sizeof(*model));
	model->world = world;
	model->settings = *settings;
}

void wreck_model_set_water(wreck_model_t *model, wreck_water_function_t *water_function, void *user)
{
	model->water_function = water_function;
	model->water_user = user;
}

void wreck_model_destroy(wreck_model_t *model)
{
	int i;

	for (i = 0; i < WRECK_PIECES_MAX; i++)
	{
		if (model->groups[i].used && b3Body_IsValid(model->groups[i].body))
		{
			b3DestroyBody(model->groups[i].body);
		}
	}
	memset(model, 0, sizeof(*model));
}

// A piece's half sizes along its own x, y and z (a log is as thick and
// as wide as its radius)
static void piece_half_sizes(const wreck_piece_t *piece, float half_sizes[3])
{
	if (piece->shape == WRECK_PIECE_BOX)
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
static void measure_piece(const wreck_model_t *model, wreck_piece_t *piece)
{
	float half_sizes[3];
	int longest = 0;
	float widest_across = 0.0f;
	int k;

	piece_half_sizes(piece, half_sizes);
	if (piece->shape == WRECK_PIECE_BOX)
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
// newton second meter) of its material's strength, against a push from
// a direction (in the building's frame; only its part across the piece
// counts): how deep the piece is that way, squared, times how wide. A
// board bears far more pushed on its edge than on its face.
static float piece_section(const wreck_piece_t *piece, b3Vec3 direction)
{
	float half_sizes[3];
	float inverse_squared = 0.0f;
	float across_squared = 0.0f;
	int k;

	piece_half_sizes(piece, half_sizes);
	for (k = 0; k < 3; k++)
	{
		b3Vec3 own = { k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f };
		int other = 3 - k - piece->long_axis;
		float part;
		float section;

		if (k == piece->long_axis)
		{
			continue;
		}
		// (Pushed along this one of its two sides across: deep along it,
		// wide along the other)
		part = b3Dot(direction, b3RotateVector(piece->local.q, own));
		section = 8.0f * half_sizes[k] * half_sizes[k] * half_sizes[other];
		inverse_squared += part * part / (section * section);
		across_squared += part * part;
	}
	if (across_squared < 1e-12f)
	{
		// (A push along the piece: as if on its face)
		float thin = half_sizes[(piece->long_axis + 1) % 3];
		float wide = half_sizes[(piece->long_axis + 2) % 3];

		if (thin > wide)
		{
			float swap = thin;

			thin = wide;
			wide = swap;
		}
		return 8.0f * thin * thin * wide;
	}
	return sqrtf(across_squared / inverse_squared);
}

// A long piece's direction in the building's frame
static b3Vec3 piece_axis(const wreck_piece_t *piece)
{
	b3Vec3 own = make_vector(piece->long_axis == 0 ? 1.0f : 0.0f, piece->long_axis == 1 ? 1.0f : 0.0f, piece->long_axis == 2 ? 1.0f : 0.0f);

	return b3RotateVector(piece->local.q, own);
}

static int add_piece(wreck_model_t *model, wreck_piece_shape_t shape, b3Vec3 position, b3Quat rotation, wreck_material_t material, uint32_t color)
{
	wreck_piece_t *piece;

	if (model->finished || model->piece_count >= WRECK_PIECES_MAX)
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

int wreck_model_add_box(wreck_model_t *model, b3Vec3 position, b3Quat rotation, b3Vec3 half_extents, wreck_material_t material, uint32_t color)
{
	int piece_index = add_piece(model, WRECK_PIECE_BOX, position, rotation, material, color);

	if (piece_index >= 0)
	{
		model->pieces[piece_index].half_extents = half_extents;
		measure_piece(model, &model->pieces[piece_index]);
	}
	return piece_index;
}

int wreck_model_add_log(wreck_model_t *model, b3Vec3 position, b3Quat rotation, float radius, float half_length, wreck_material_t material, uint32_t color)
{
	int piece_index = add_piece(model, WRECK_PIECE_LOG, position, rotation, material, color);

	if (piece_index >= 0)
	{
		model->pieces[piece_index].radius = radius;
		model->pieces[piece_index].half_length = half_length;
		measure_piece(model, &model->pieces[piece_index]);
	}
	return piece_index;
}

// Which way a link's joint faces, from piece_b towards piece_a (see
// wreck.h)
static b3Vec3 link_normal(const wreck_model_t *model, int piece_a, int piece_b, b3Vec3 point)
{
	b3Vec3 from_point = b3Sub(model->pieces[piece_a].local.p, point);
	b3Vec3 level;
	float length;

	if (piece_b == WRECK_GROUND)
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

int wreck_model_add_link(wreck_model_t *model, int piece_a, int piece_b, b3Vec3 point, wreck_link_type_t type)
{
	wreck_link_t *link;

	if (model->finished || model->link_count >= WRECK_LINKS_MAX || piece_a < 0 || piece_a >= model->piece_count
		|| piece_b >= model->piece_count || (piece_b < 0 && piece_b != WRECK_GROUND) || piece_a == piece_b)
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
	link->hit_points_left = 1.0f;
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
static void add_piece_shape(const wreck_model_t *model, b3BodyId body, int piece_index)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	b3ShapeDef definition = b3DefaultShapeDef();
	float gap = model->settings.shape_gap;

	definition.density = model->settings.densities[piece->material];
	definition.baseMaterial.friction = FRICTION;
	definition.enableHitEvents = true;
	definition.userData = (void *)(intptr_t)(piece_index + 1);
	if (piece->shape == WRECK_PIECE_BOX)
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
static void create_group_body(wreck_model_t *model, int group_index, b3Transform frame)
{
	wreck_group_t *group = &model->groups[group_index];
	b3BodyDef definition = b3DefaultBodyDef();
	int i;

	definition.type = group->anchored ? b3_staticBody : b3_dynamicBody;
	definition.position = frame.p;
	definition.rotation = frame.q;
	// (The model itself: wreck_body_model. Which group a body is comes
	// from its shapes' pieces.)
	definition.userData = model;
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
static int allocate_group(wreck_model_t *model)
{
	int i;

	for (i = 0; i < WRECK_PIECES_MAX; i++)
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
static void label_island(wreck_model_t *model, int first_piece, int group_index)
{
	wreck_group_t *group = &model->groups[group_index];
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
			const wreck_link_t *link = &model->links[model->piece_links[k]];
			int other;

			if (link->broken)
			{
				continue;
			}
			if (link->piece_b == WRECK_GROUND)
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
static void list_piece_links(wreck_model_t *model)
{
	int fill[WRECK_PIECES_MAX];
	int i;

	memset(model->piece_link_start, 0, sizeof(model->piece_link_start));
	for (i = 0; i < model->link_count; i++)
	{
		model->piece_link_start[model->links[i].piece_a + 1]++;
		if (model->links[i].piece_b != WRECK_GROUND)
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
		if (model->links[i].piece_b != WRECK_GROUND)
		{
			model->piece_links[fill[model->links[i].piece_b]++] = i;
		}
	}
}

void wreck_model_finish(wreck_model_t *model)
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

void wreck_model_add_projectile(wreck_model_t *model, b3BodyId body, float radius)
{
	wreck_projectile_t *projectile;

	if (model->projectile_count >= WRECK_PROJECTILES_MAX)
	{
		return;
	}
	projectile = &model->projectiles[model->projectile_count];
	memset(projectile, 0, sizeof(*projectile));
	projectile->body = body;
	projectile->radius = radius;
	model->projectile_count++;
}

void wreck_model_remove_projectile(wreck_model_t *model, b3BodyId body)
{
	int index = find_projectile(model, body);

	if (index >= 0)
	{
		model->projectiles[index] = model->projectiles[model->projectile_count - 1];
		model->projectile_count--;
	}
}

// ---------------------------------------------------------------------
// Groups made anew

// One group after links of it broke: its islands found again. If it is
// still one island on the same footing nothing changes; else its body is
// destroyed and each island gets its own, where the old one was and
// moving as that part of it did.
static void rebuild_group(wreck_model_t *model, int old_group_index)
{
	wreck_group_t old_group = model->groups[old_group_index];
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
		wreck_group_t *group = &model->groups[group_parts[0]];

		group->body = old_group.body;
		group->rubble = old_group.rubble;
		group->linear_velocity_before_step = old_group.linear_velocity_before_step;
		group->angular_velocity_before_step = old_group.angular_velocity_before_step;
		// (its user data stays the model's pointer: wreck_body_model)
		return;
	}

	b3DestroyBody(old_group.body);
	for (i = 0; i < part_count; i++)
	{
		wreck_group_t *group = &model->groups[group_parts[i]];

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

static void rebuild_dirty_groups(wreck_model_t *model)
{
	int i;

	if (!model->any_group_dirty)
	{
		return;
	}
	model->any_group_dirty = false;
	// (A group made while this runs takes a free number: it is never dirty)
	for (i = 0; i < WRECK_PIECES_MAX; i++)
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

static void break_link(wreck_model_t *model, int link_index, int frame_index)
{
	wreck_link_t *link = &model->links[link_index];

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

// How much of a link a sudden push uses up (1: all of it), the push
// given as its parts through the link, newton seconds: pulling it apart,
// pressing it together, sliding it, and turning it (newton meter
// seconds).
//
// A link lets go when it has taken up so much work: what it bears (the
// newtons of the support pass, by the way it is strained) times how far
// it gives before it lets go (give, meters). A push of so many newton
// seconds between two pieces sets them moving against each other with
// the push squared over twice their reduced mass in joules, and that is
// the work the link must take up: so a nail that holds a light board
// lets go at a far smaller push than one that holds a log. Turning is
// taken as a push at joint_arm. Of what it can take, the first part
// (link_elastic) springs back and costs it nothing.
static float link_harm(const wreck_model_t *model, const wreck_link_t *link, double pulled, double pressed, double slid, double turned)
{
	const wreck_link_settings_t *limits = &model->settings.links[link->type];
	double mass = (double)model->pieces[link->piece_a].mass;
	double grip = (double)limits->grip * (double)link->static_compression;
	double turn_push = turned / (double)model->settings.joint_arm;
	double used;

	if (link->piece_b != WRECK_GROUND)
	{
		double other_mass = (double)model->pieces[link->piece_b].mass;

		mass = mass * other_mass / (mass + other_mass);
	}
	used = (pulled * pulled / (double)limits->pull
		+ pressed * pressed / (double)limits->compression
		+ slid * slid / ((double)limits->slide + grip)
		+ turn_push * turn_push / (((double)limits->cantilever + grip * (double)model->settings.joint_arm) / (double)model->settings.joint_arm))
		/ (2.0 * mass * (double)limits->give);
	return used > (double)model->settings.link_elastic ? (float)(used - (double)model->settings.link_elastic) : 0.0f;
}

// The same, from how the solver left the link's two pieces (link_strain)
static float link_harm_from_strain(const wreck_model_t *model, const wreck_link_t *link, const double apart[3], const double turned[3])
{
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	double along = apart[0] * (double)link->local_normal.x + apart[1] * (double)link->local_normal.y + apart[2] * (double)link->local_normal.z;
	double across_x = apart[0] - along * (double)link->local_normal.x;
	double across_y = apart[1] - along * (double)link->local_normal.y;
	double across_z = apart[2] - along * (double)link->local_normal.z;

	// (piece_a come nearer piece_b: pressed together)
	return link_harm(model, link, along > 0.0 ? along : 0.0, along < 0.0 ? -along : 0.0,
		sqrt(across_x * across_x + across_y * across_y + across_z * across_z),
		turn_stiffness * sqrt(turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2]));
}

// Takes that much of a link (link_harm). True when that broke it.
static bool hurt_link(wreck_model_t *model, int link_index, float harm, int frame_index)
{
	wreck_link_t *link = &model->links[link_index];

	if (link->broken || harm <= 0.0f)
	{
		return false;
	}
	link->hit_points_left -= harm;
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
static bool hurt_piece(wreck_model_t *model, int piece_index, float impulse, int frame_index)
{
	wreck_piece_t *piece = &model->pieces[piece_index];
	float threshold = model->settings.piece_threshold[piece->material];
	int k;

	if (piece->group == WRECK_GONE || piece->crushed || impulse <= threshold)
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
static bool request_cut(wreck_model_t *model, int piece_index, float position)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	b3Vec3 axis;
	float half_sizes[3];
	float furthest;
	int i;
	int k;

	if (piece->long_axis < 0 || model->piece_count + model->pending_cut_count >= WRECK_PIECES_MAX)
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
		const wreck_link_t *link = &model->links[model->piece_links[k]];
		const wreck_piece_t *other;
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
static void cut_piece(wreck_model_t *model, int piece_index, float position)
{
	wreck_piece_t *piece = &model->pieces[piece_index];
	wreck_piece_t *other_part = &model->pieces[model->piece_count];
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
	if (piece->shape == WRECK_PIECE_BOX)
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
		wreck_link_t *link = &model->links[i];

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
static void make_pending_cuts(wreck_model_t *model)
{
	int i;

	if (model->pending_cut_count == 0)
	{
		return;
	}
	for (i = 0; i < model->pending_cut_count; i++)
	{
		if (model->pieces[model->pending_cut_pieces[i]].group != WRECK_GONE && model->piece_count < WRECK_PIECES_MAX)
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
static bool snap_piece(wreck_model_t *model, int piece_index, b3Vec3 world_point, b3Vec3 world_direction, float impulse)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
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

	if (piece->group == WRECK_GONE || piece->long_axis < 0)
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
		const wreck_link_t *link = &model->links[model->piece_links[k]];
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
	if (bending <= model->settings.snap[piece->material] * piece_section(piece, direction))
	{
		return false;
	}
	return request_cut(model, piece_index, position);
}

// An impulse on a piece: shared evenly among its whole links; each link
// passes what it got, times hop_falloff, to the piece at its other end,
// which shares that among its own. True when a link broke.
static bool share_hit(wreck_model_t *model, int piece_index, float impulse, int frame_index)
{
	bool broke = false;
	int queue_start = 0;
	int queue_end = 0;
	int k;

	if (model->pieces[piece_index].group == WRECK_GONE || model->groups[model->pieces[piece_index].group].rubble)
	{
		return false;
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
			const wreck_link_t *link = &model->links[link_index];
			int other;
			float passed_on = share * model->settings.hop_falloff;

			// (A link is hurt once a hit, from the side reached first)
			if (link->broken || walk_link_stamps[link_index] == walk_stamp)
			{
				continue;
			}
			walk_link_stamps[link_index] = walk_stamp;
			// (Which way the share goes through the link is not known
			// here: taken as sliding it)
			if (hurt_link(model, link_index, link_harm(model, link, 0.0, 0.0, (double)share, 0.0), frame_index))
			{
				broke = true;
			}
			if (link->piece_b == WRECK_GROUND)
			{
				continue;
			}
			other = link->piece_a == current ? link->piece_b : link->piece_a;
			if (walk_piece_stamps[other] != walk_stamp && passed_on > HIT_SHARE_MIN)
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
static void link_strain(const wreck_model_t *model, const wreck_link_t *link, const double *unknowns, double apart[3], double turned[3])
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
	if (link->piece_b != WRECK_GROUND)
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
static void springs_product(const wreck_model_t *model, const double *unknowns, double *product, int unknown_count)
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
		const wreck_link_t *link = &model->links[i];
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

			if (piece_index == WRECK_GROUND)
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
	for (i = 0; i < unknown_count; i++)
	{
		product[i] += solver_inertia[i] * unknowns[i];
	}
}

// How bent a long piece is, as many times what it bears, and where
// along it that is worst. Its links' forces on it (from where the
// solver left the pieces: unknowns) and what is spread along it
// (own_load: its weight, or a blast's push), taken up to a place along
// it, turn about that place: the part of that turning across the piece
// is the bending there. strength: its material's, against this kind of
// load (bend or snap).
static double piece_bending(const wreck_model_t *model, int piece_index, const double *unknowns, const double own_load[3], double strength, double *worst_position_result)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	double load_positions[BEND_LOADS_MAX];
	double load_forces[BEND_LOADS_MAX][3];
	double load_moments[BEND_LOADS_MAX][3];
	double axis[3];
	float half_sizes[3];
	double half_length;
	double worst_bent = 0.0;
	double worst_position = 0.0;
	int load_count = 0;
	int sample;
	int k;

	{
		b3Vec3 direction = piece_axis(piece);

		axis[0] = (double)direction.x;
		axis[1] = (double)direction.y;
		axis[2] = (double)direction.z;
	}
	piece_half_sizes(piece, half_sizes);
	half_length = (double)half_sizes[piece->long_axis];

	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1] && load_count < BEND_LOADS_MAX; k++)
	{
		const wreck_link_t *link = &model->links[model->piece_links[k]];
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
		link_strain(model, link, unknowns, apart, turned);
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
		double part_share;
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
		// ...and that part's share of what is spread along the piece (its
		// weight, or a blast's push), at the part's own middle
		part_share = (position + half_length) / (2.0 * half_length);
		part_middle = (position - half_length) * 0.5;
		for (k = 0; k < 3; k++)
		{
			force[k] += own_load[k] * part_share;
		}
		moment[0] += part_middle * part_share * (axis[1] * own_load[2] - axis[2] * own_load[1]);
		moment[1] += part_middle * part_share * (axis[2] * own_load[0] - axis[0] * own_load[2]);
		moment[2] += part_middle * part_share * (axis[0] * own_load[1] - axis[1] * own_load[0]);
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
		if (bending > 1e-9)
		{
			// (What turns it so pushes it across both the turning and
			// its length: that way is how deep it must be)
			b3Vec3 push = {
				(float)((axis[1] * turning[2] - axis[2] * turning[1]) / bending),
				(float)((axis[2] * turning[0] - axis[0] * turning[2]) / bending),
				(float)((axis[0] * turning[1] - axis[1] * turning[0]) / bending) };
			double bent = bending / (strength * (double)piece_section(piece, push));

			if (bent > worst_bent)
			{
				worst_bent = bent;
				worst_position = position;
			}
		}
	}
	*worst_position_result = worst_position;
	return worst_bent;
}

// What stands: every piece a walk over whole links reaches from one a
// whole link holds to the ground (the rest is loose, Box3D's business).
// Each gets its place among the solver's unknowns, in support_unknowns
// (-1: it does not stand), and walk_queue lists them. How many there are.
static int find_standing(const wreck_model_t *model)
{
	int queue_start = 0;
	int queue_end = 0;
	int i;
	int k;

	for (i = 0; i < model->piece_count; i++)
	{
		support_unknowns[i] = -1;
	}
	for (i = 0; i < model->link_count; i++)
	{
		const wreck_link_t *link = &model->links[i];

		if (!link->broken && link->piece_b == WRECK_GROUND && support_unknowns[link->piece_a] < 0)
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
			const wreck_link_t *link = &model->links[model->piece_links[k]];
			int other;

			if (link->broken || link->piece_b == WRECK_GROUND)
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
	return queue_end;
}

// Where the standing pieces settle with every link a spring, under the
// loads in solver_load, starting from solver_x and leaving the answer
// there: conjugate gradients. How many rounds it took.
static int solve_springs(const wreck_model_t *model, int standing_count, double load_squared)
{
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	int unknown_count = standing_count * 6;
	double residual_scaled = 0.0;
	int round;
	int i;

	// How stiffly each unknown is held by its own links alone, which the
	// solver divides by to be at home with turns and moves alike
	for (i = 0; i < unknown_count; i++)
	{
		solver_diagonal[i] = solver_inertia[i];
	}
	for (i = 0; i < model->link_count; i++)
	{
		const wreck_link_t *link = &model->links[i];
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

			if (piece_index == WRECK_GROUND)
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
	return round;
}

// The support pass: where everything that stands settles under its own
// weight with every link a spring (see wreck.h), found by
// conjugate gradients from where it settled last time; then what every
// link bears against what it can. Pieces pressed harder than their
// material bears are crushed: their links break.
static void water_on_standing(const wreck_model_t *model, int piece_index, double load[6]);

static void work_out_support(wreck_model_t *model, int frame_index)
{
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	int queue_end = 0;
	int unknown_count;
	double load_squared = 0.0;
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

	queue_end = find_standing(model);
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
		}
		// (The water on it: its lift, and the push and the turning of its
		// flow)
		water_on_standing(model, piece_index, &solver_load[i * 6]);
		for (k = 0; k < 6; k++)
		{
			model->water_loads[piece_index][k] = (float)solver_load[i * 6 + k];
		}
		solver_load[i * 6 + 1] -= (double)model->pieces[piece_index].mass * (double)GRAVITY;
		load_squared += solver_load[i * 6 + 0] * solver_load[i * 6 + 0] + solver_load[i * 6 + 1] * solver_load[i * 6 + 1] + solver_load[i * 6 + 2] * solver_load[i * 6 + 2];
	}
	for (i = 0; i < unknown_count; i++)
	{
		solver_inertia[i] = 0.0;
	}
	model->support_iterations = solve_springs(model, queue_end, load_squared);
	for (i = 0; i < queue_end; i++)
	{
		memcpy(&model->support_solution[walk_queue[i] * 6], &solver_x[i * 6], 6 * sizeof(double));
		piece_compressions[walk_queue[i]] = 0.0f;
	}

	// What every link bears: along its direction (pressed together or
	// pulled apart), across it, and turning; each against its limit
	for (i = 0; i < model->link_count; i++)
	{
		wreck_link_t *link = &model->links[i];
		const wreck_link_settings_t *limits = &model->settings.links[link->type];
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
		if (link->piece_b != WRECK_GROUND)
		{
			piece_compressions[link->piece_b] += compression;
		}
	}

	// The long pieces: where one is bent by more than it bears, it is cut
	for (i = 0; i < queue_end; i++)
	{
		int piece_index = walk_queue[i];
		wreck_piece_t *piece = &model->pieces[piece_index];
		// (Its own weight and the water on it, taken as spread evenly
		// along it)
		double own_weight[3] = { (double)model->water_loads[piece_index][0], (double)model->water_loads[piece_index][1] - (double)piece->mass * (double)GRAVITY, (double)model->water_loads[piece_index][2] };
		double worst_position = 0.0;
		double worst_bent;

		piece->bent = 0.0f;
		if (piece->long_axis < 0)
		{
			continue;
		}
		worst_bent = piece_bending(model, piece_index, solver_x, own_weight, (double)model->settings.bend[piece->material], &worst_position);
		piece->bent = (float)worst_bent;
		if (worst_bent > 1.0)
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
static void remove_crushed(wreck_model_t *model)
{
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		wreck_piece_t *piece = &model->pieces[i];

		if (piece->crushed && piece->group != WRECK_GONE && model->groups[piece->group].piece_count == 1)
		{
			b3DestroyBody(model->groups[piece->group].body);
			model->groups[piece->group].used = false;
			piece->group = WRECK_GONE;
		}
	}
}

// The overloaded links give way a little more: each loses hit points as
// fast as it is overloaded (one at just its limit lasts
// collapse_seconds from whole)
static void give_way(wreck_model_t *model, int frame_index, float step_seconds)
{
	int i;

	for (i = 0; i < model->overloaded_count; i++)
	{
		int link_index = model->overloaded_links[i];
		wreck_link_t *link = &model->links[link_index];

		if (link->broken)
		{
			continue;
		}
		link->hit_points_left -= link->overload * step_seconds / model->settings.collapse_seconds;
		if (link->hit_points_left <= 0.0f)
		{
			break_link(model, link_index, frame_index);
		}
	}
}

static float exposed_area(const wreck_piece_t *piece, b3Quat rotation, b3Vec3 direction);

// The air and the water on what is loose.
//
// A piece is taken in parts (FLUID_PARTS_LONG along its longest side by
// FLUID_PARTS_WIDE along the next), each with its own place, speed and
// depth in the water:
//   - what of the part is under water is borne up by the weight of the
//     water it takes the place of;
//   - a part at the surface that goes up or down is slowed by the waves
//     it makes;
//   - the water pushes back on that much of it, and the air on the rest,
//     by how much of it faces the way it goes through them and by its
//     speed through them squared (the water flows, the air stands
//     still), the water as many times harder as it is denser.
// Nothing is done to the turning as such: a board that spins has parts
// that move through the water, and those are slowed, about every axis
// that has parts off it (a round log spinning about its own length is
// not slowed, and little would slow it). A log floats lying down and a
// board flat because the parts that are deeper are borne up more.
//
// Each part's push is worked out so that it can never take more than all
// of the part's own speed through the fluid, however long the step: it
// cannot fling anything.
// A piece laid out in parts for the air and the water: where the parts
// are in the world and how tall the piece and each part stand as they
// lie
typedef struct
{
	b3Transform transform;
	b3Vec3 places[FLUID_PARTS_LONG * FLUID_PARTS_WIDE];
	int part_count;
	float half_height;
	float part_half_height;
} fluid_parts_t;

static void lay_out_fluid_parts(const wreck_model_t *model, int piece_index, fluid_parts_t *parts)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	b3Vec3 axes[3];
	float half_sizes[3];
	int part_counts[3] = { 1, 1, 1 };
	int longest = 0;
	int shortest = 0;
	int part_x;
	int part_y;
	int part_z;
	int k;

	parts->transform = wreck_model_piece_transform(model, piece_index);
	piece_half_sizes(piece, half_sizes);
	for (k = 0; k < 3; k++)
	{
		b3Vec3 own = { k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f };

		axes[k] = b3RotateVector(parts->transform.q, own);
		if (half_sizes[k] > half_sizes[longest])
		{
			longest = k;
		}
		if (half_sizes[k] < half_sizes[shortest])
		{
			shortest = k;
		}
	}
	if (longest == shortest)
	{
		shortest = (longest + 1) % 3;
	}
	part_counts[longest] = FLUID_PARTS_LONG;
	part_counts[3 - longest - shortest] = FLUID_PARTS_WIDE;
	parts->half_height = 0.0f;
	parts->part_half_height = 0.0f;
	for (k = 0; k < 3; k++)
	{
		parts->half_height += fabsf(axes[k].y) * half_sizes[k];
		parts->part_half_height += fabsf(axes[k].y) * half_sizes[k] / (float)part_counts[k];
	}
	parts->part_count = 0;
	for (part_x = 0; part_x < part_counts[0]; part_x++)
	{
		for (part_y = 0; part_y < part_counts[1]; part_y++)
		{
			for (part_z = 0; part_z < part_counts[2]; part_z++)
			{
				int indices[3] = { part_x, part_y, part_z };
				b3Vec3 place = parts->transform.p;

				for (k = 0; k < 3; k++)
				{
					place = b3Add(place, b3MulSV(((2.0f * (float)indices[k] + 1.0f) / (float)part_counts[k] - 1.0f) * half_sizes[k], axes[k]));
				}
				parts->places[parts->part_count++] = place;
			}
		}
	}
}

// How much of something that reaches half_height up and down from
// place.y is under the water there (0 none, 1 all), and how the water
// flows there
static float share_under_water(const wreck_model_t *model, b3Vec3 place, float half_height, b3Vec3 *flow)
{
	float surface_height = 0.0f;
	float under_water;

	*flow = b3Vec3_zero;
	if (model->water_function == NULL || !model->water_function(model->water_user, place, &surface_height, flow))
	{
		*flow = b3Vec3_zero;
		return 0.0f;
	}
	under_water = (surface_height - (place.y - half_height)) / (2.0f * half_height);
	return under_water < 0.0f ? 0.0f : (under_water > 1.0f ? 1.0f : under_water);
}

// The water on a piece that stands, as a load for the support pass, in
// the building's frame: the force (newtons: the lift on what of it is
// under water, and the push of the flow, by the same law that slows a
// loose piece moving through still water) and its turning about the
// piece's middle (newton meters)
static void water_on_standing(const wreck_model_t *model, int piece_index, double load[6])
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	float water_drag = model->settings.air_drag * model->settings.water_density / model->settings.air_density;
	b3Vec3 force = b3Vec3_zero;
	b3Vec3 turning = b3Vec3_zero;
	b3Quat frame_rotation;
	fluid_parts_t parts;
	float part_volume;
	int part;
	int k;

	for (k = 0; k < 6; k++)
	{
		load[k] = 0.0;
	}
	if (model->water_function == NULL)
	{
		return;
	}
	lay_out_fluid_parts(model, piece_index, &parts);
	part_volume = piece->mass / model->settings.densities[piece->material] / (float)parts.part_count;
	for (part = 0; part < parts.part_count; part++)
	{
		b3Vec3 flow;
		float under_water = share_under_water(model, parts.places[part], parts.part_half_height, &flow);
		b3Vec3 part_force = { 0.0f, model->settings.water_density * GRAVITY * part_volume * under_water, 0.0f };
		float speed = b3Length(flow);

		if (under_water <= 0.0f)
		{
			continue;
		}
		if (speed > 1e-4f)
		{
			float area = under_water * exposed_area(piece, parts.transform.q, b3MulSV(1.0f / speed, flow)) / (float)parts.part_count;

			part_force = b3Add(part_force, b3MulSV(water_drag * area * speed, flow));
		}
		force = b3Add(force, part_force);
		turning = b3Add(turning, b3Cross(b3Sub(parts.places[part], parts.transform.p), part_force));
	}
	frame_rotation = b3Body_GetRotation(model->groups[piece->group].body);
	force = b3InvRotateVector(frame_rotation, force);
	turning = b3InvRotateVector(frame_rotation, turning);
	load[0] = (double)force.x;
	load[1] = (double)force.y;
	load[2] = (double)force.z;
	load[3] = (double)turning.x;
	load[4] = (double)turning.y;
	load[5] = (double)turning.z;
}

// Has the water on what stands changed enough, since who bears what was
// last worked out, for it to be worked out anew?
static void watch_water_on_standing(wreck_model_t *model)
{
	int i;
	int k;

	if (model->water_function == NULL || model->support_dirty)
	{
		return;
	}
	for (i = 0; i < model->piece_count; i++)
	{
		const wreck_piece_t *piece = &model->pieces[i];
		double load[6];
		double change_squared = 0.0;
		double enough = (double)(WATER_LOAD_CHANGE * piece->mass * GRAVITY);

		if (piece->group == WRECK_GONE || !model->groups[piece->group].anchored)
		{
			continue;
		}
		water_on_standing(model, i, load);
		for (k = 0; k < 3; k++)
		{
			change_squared += (load[k] - (double)model->water_loads[i][k]) * (load[k] - (double)model->water_loads[i][k]);
		}
		if (change_squared > enough * enough)
		{
			model->support_dirty = true;
			return;
		}
	}
}

static void push_by_fluids(wreck_model_t *model, float step_seconds)
{
	float water_drag = model->settings.air_drag * model->settings.water_density / model->settings.air_density;
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		wreck_piece_t *piece = &model->pieces[i];
		const wreck_group_t *group;
		fluid_parts_t parts;
		float wet = 0.0f;
		float part_mass;
		float part_volume;
		int part;

		if (piece->group == WRECK_GONE)
		{
			continue;
		}
		group = &model->groups[piece->group];
		if (group->anchored)
		{
			continue;
		}
		lay_out_fluid_parts(model, i, &parts);
		// How much of the piece as a whole is under water. What sleeps is
		// left asleep (or nothing would ever sleep in water) unless the
		// water flows past it, or has risen or fallen along it since it
		// was last awake by enough to matter against its weight
		if (model->water_function != NULL)
		{
			b3Vec3 flow;

			wet = share_under_water(model, parts.transform.p, parts.half_height, &flow);
			if (!b3Body_IsAwake(group->body))
			{
				if (b3Length(flow) < WATER_WAKE_SPEED && fabsf(wet - piece->wet) * model->settings.water_density < WATER_WAKE_LIFT * model->settings.densities[piece->material])
				{
					continue;
				}
				b3Body_SetAwake(group->body, true);
			}
		}
		else if (!b3Body_IsAwake(group->body))
		{
			continue;
		}
		piece->wet = wet;
		part_mass = piece->mass / (float)parts.part_count;
		part_volume = part_mass / model->settings.densities[piece->material];

		for (part = 0; part < parts.part_count; part++)
		{
			b3Vec3 place = parts.places[part];
			b3Vec3 velocity = b3Body_GetWorldPointVelocity(group->body, place);
			b3Vec3 push = b3Vec3_zero;
			b3Vec3 flow;
			float under_water = share_under_water(model, place, parts.part_half_height, &flow);
			float speed;

			if (under_water > 0.0f)
			{
				b3Vec3 through_water = b3Sub(velocity, flow);
				b3Vec3 lift = { 0.0f, model->settings.water_density * GRAVITY * part_volume * under_water, 0.0f };

				// (A force, not a push: it goes into the next step side
				// by side with the weight, so what floats at rest has no
				// speed and can fall asleep)
				b3Body_ApplyForce(group->body, lift, place, false);
				if (under_water < 1.0f)
				{
					// Waves: a part at the surface that goes up or down
					// makes waves, which carry its movement away. The
					// water pushes back on its area at the surface by its
					// speed up or down and by the speed of the waves it
					// makes, which is that of waves in water as deep as
					// the part is sunk: sqrt(gravity * depth sunk). It is
					// what stops a floating board bobbing and rocking
					// after a swing or two
					float depth_sunk = under_water * 2.0f * parts.part_half_height;
					float area_at_surface = part_volume / (2.0f * parts.part_half_height);
					float slowing = model->settings.water_density * area_at_surface * sqrtf(GRAVITY * depth_sunk) * step_seconds / part_mass;

					push.y -= part_mass * slowing / (1.0f + slowing) * through_water.y;
				}
				speed = b3Length(through_water);
				if (speed > 1e-4f)
				{
					float area = under_water * exposed_area(piece, parts.transform.q, b3MulSV(1.0f / speed, through_water)) / (float)parts.part_count;
					float slowing = water_drag * area * speed * step_seconds / part_mass;

					push = b3Add(push, b3MulSV(-part_mass * slowing / (1.0f + slowing), through_water));
				}
			}
			speed = b3Length(velocity);
			if (under_water < 1.0f && speed >= AIR_DRAG_SPEED_MIN)
			{
				float area = (1.0f - under_water) * exposed_area(piece, parts.transform.q, b3MulSV(1.0f / speed, velocity)) / (float)parts.part_count;
				float slowing = model->settings.air_drag * area * speed * step_seconds / part_mass;

				push = b3Add(push, b3MulSV(-part_mass * slowing / (1.0f + slowing), velocity));
			}
			if (push.x != 0.0f || push.y != 0.0f || push.z != 0.0f)
			{
				b3Body_ApplyLinearImpulse(group->body, push, place, false);
			}
		}
	}
}

void wreck_push_ball_by_fluids(const wreck_settings_t *settings, wreck_water_function_t *water_function, void *water_user, b3BodyId body, float radius, float step_seconds)
{
	b3Vec3 place = b3Body_GetWorldCenter(body);
	b3Vec3 velocity = b3Body_GetLinearVelocity(body);
	b3Vec3 flow = b3Vec3_zero;
	b3Vec3 push = b3Vec3_zero;
	float mass = b3Body_GetMass(body);
	float face = PI * radius * radius;
	float surface_height = 0.0f;
	float depth_sunk = 0.0f;
	float under_water;
	float lift;
	float speed;

	if (mass <= 0.0f)
	{
		return;
	}
	if (water_function != NULL && water_function(water_user, place, &surface_height, &flow))
	{
		depth_sunk = surface_height - (place.y - radius);
		depth_sunk = depth_sunk < 0.0f ? 0.0f : (depth_sunk > 2.0f * radius ? 2.0f * radius : depth_sunk);
	}
	// (The share of a ball's volume below a level: that of its cap)
	under_water = depth_sunk * depth_sunk * (3.0f * radius - depth_sunk) / (4.0f * radius * radius * radius);
	lift = settings->water_density * GRAVITY * under_water * 4.0f / 3.0f * PI * radius * radius * radius;
	if (!b3Body_IsAwake(body))
	{
		// As with the pieces: a ball asleep is woken by water that flows
		// or that would bear it up
		if (b3Length(flow) < WATER_WAKE_SPEED && lift < mass * GRAVITY)
		{
			return;
		}
		b3Body_SetAwake(body, true);
	}
	if (under_water > 0.0f)
	{
		b3Vec3 through_water = b3Sub(velocity, flow);
		b3Vec3 lift_force = { 0.0f, lift, 0.0f };

		b3Body_ApplyForceToCenter(body, lift_force, false);
		if (under_water < 1.0f)
		{
			// (Waves, as for a piece at the surface)
			float area_at_surface = PI * depth_sunk * (2.0f * radius - depth_sunk);
			float slowing = settings->water_density * area_at_surface * sqrtf(GRAVITY * depth_sunk) * step_seconds / mass;

			push.y -= mass * slowing / (1.0f + slowing) * through_water.y;
		}
		speed = b3Length(through_water);
		if (speed > 1e-4f)
		{
			float water_drag = BALL_DRAG_SHARE * settings->air_drag * settings->water_density / settings->air_density;
			float slowing = water_drag * under_water * face * speed * step_seconds / mass;

			push = b3Add(push, b3MulSV(-mass * slowing / (1.0f + slowing), through_water));
		}
	}
	speed = b3Length(velocity);
	if (under_water < 1.0f && speed >= AIR_DRAG_SPEED_MIN)
	{
		float slowing = BALL_DRAG_SHARE * settings->air_drag * (1.0f - under_water) * face * speed * step_seconds / mass;

		push = b3Add(push, b3MulSV(-mass * slowing / (1.0f + slowing), velocity));
	}
	if (push.x != 0.0f || push.y != 0.0f || push.z != 0.0f)
	{
		b3Body_ApplyLinearImpulseToCenter(body, push, false);
	}
}

void wreck_model_before_step(wreck_model_t *model)
{
	int i;

	for (i = 0; i < WRECK_PIECES_MAX; i++)
	{
		wreck_group_t *group = &model->groups[i];

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
static void keep_going(wreck_model_t *model, b3BodyId body)
{
	float kept = model->settings.breakthrough_speed_kept;

	if (!b3Body_IsValid(body))
	{
		return;
	}
	if (wreck_body_model(body) == model)
	{
		b3ShapeId shape;
		const wreck_group_t *group;

		b3Body_GetShapes(body, &shape, 1);
		group = &model->groups[model->pieces[wreck_model_shape_piece(model, shape)].group];
		if (group->used && !group->anchored && !group->rebuilt)
		{
			b3Body_SetLinearVelocity(body, b3MulSV(kept, group->linear_velocity_before_step));
			b3Body_SetAngularVelocity(body, group->angular_velocity_before_step);
		}
	}
	else
	{
		// (Only the bodies it was told are projectiles: anything else of
		// the world's is left alone)
		int index = find_projectile(model, body);

		if (index >= 0)
		{
			b3Body_SetLinearVelocity(body, b3MulSV(kept, model->projectiles[index].linear_velocity_before_step));
			b3Body_SetAngularVelocity(body, model->projectiles[index].angular_velocity_before_step);
		}
	}
}

// A loose group has hit something and was stopped short: the shock of it
// through the whole group. Each piece was slowed by so much in the step
// (less what gravity did), and what slowed it came through its links
// from the pieces that did the hitting: so each piece is pushed, in the
// group's own frame, by its mass times the speed it lost, the pieces
// that hit are held where they are, and the springs solved, as for a
// blast (wreck_model_blast). What goes through a link hurts it, and a
// long piece bent by more than it bears from a hit is cut. A roof that
// lands on one corner breaks where its own weight bends it, not only at
// the corner.
static void shock_through_group(wreck_model_t *model, int group_index, int frame_index, float step_seconds)
{
	const wreck_group_t *group = &model->groups[group_index];
	double turn_stiffness = (double)model->settings.joint_arm * (double)model->settings.joint_arm;
	b3Quat rotation = b3Body_GetRotation(group->body);
	b3Vec3 center = b3Body_GetWorldPoint(group->body, b3Body_GetLocalCenter(group->body));
	int piece_count_before = model->piece_count;
	double load_squared = 0.0;
	int count = 0;
	int i;
	int k;

	for (i = 0; i < model->piece_count; i++)
	{
		support_unknowns[i] = -1;
		if (model->pieces[i].group == group_index)
		{
			support_unknowns[i] = count;
			walk_queue[count++] = i;
		}
	}
	for (i = 0; i < count; i++)
	{
		int piece_index = walk_queue[i];
		const wreck_piece_t *piece = &model->pieces[piece_index];
		b3Vec3 place = b3Body_GetWorldPoint(group->body, piece->local.p);
		b3Vec3 speed_before = b3Add(group->linear_velocity_before_step, b3Cross(group->angular_velocity_before_step, b3Sub(place, center)));
		b3Vec3 speed_lost = b3Sub(speed_before, b3Body_GetWorldPointVelocity(group->body, place));
		b3Vec3 push;

		speed_lost.y -= GRAVITY * step_seconds;
		push = b3InvRotateVector(rotation, b3MulSV(piece->mass, speed_lost));
		for (k = 0; k < 6; k++)
		{
			solver_load[i * 6 + k] = 0.0;
			solver_x[i * 6 + k] = 0.0;
			// (A piece that hit is held as by one link at its middle)
			solver_inertia[i * 6 + k] = !model->shock_pieces[piece_index] ? 0.0 : (k < 3 ? 1.0 : turn_stiffness);
		}
		solver_load[i * 6 + 0] = (double)push.x;
		solver_load[i * 6 + 1] = (double)push.y;
		solver_load[i * 6 + 2] = (double)push.z;
		load_squared += (double)push.x * (double)push.x + (double)push.y * (double)push.y + (double)push.z * (double)push.z;
	}
	if (load_squared <= 0.0)
	{
		return;
	}
	solve_springs(model, count, load_squared);

	for (i = 0; i < model->link_count; i++)
	{
		const wreck_link_t *link = &model->links[i];
		double apart[3];
		double turned[3];

		if (link->broken || support_unknowns[link->piece_a] < 0)
		{
			continue;
		}
		link_strain(model, link, solver_x, apart, turned);
		hurt_link(model, i, link_harm_from_strain(model, link, apart, turned), frame_index);
	}
	for (i = 0; i < count; i++)
	{
		int piece_index = walk_queue[i];
		const wreck_piece_t *piece = &model->pieces[piece_index];
		double own_push[3];
		double worst_position = 0.0;

		if (piece->long_axis < 0 || piece->crushed || piece_index >= piece_count_before)
		{
			continue;
		}
		for (k = 0; k < 3; k++)
		{
			own_push[k] = solver_load[i * 6 + k] - solver_inertia[i * 6 + k] * solver_x[i * 6 + k];
		}
		if (piece_bending(model, piece_index, solver_x, own_push, (double)model->settings.snap[piece->material], &worst_position) > 1.0)
		{
			request_cut(model, piece_index, (float)worst_position);
		}
	}
}

// A long piece, loose by itself, that was hit in the step just made (it
// fell on something, something fell on it): is it bent enough to break,
// and where?
//
// The pushes of everything that touches it (the step's contact impulses,
// across its length) would move and turn it as a whole; each slice of it
// takes its share of that by its mass. What the pushes give a slice
// beyond its share has to pass along the piece, as bending. That gives
// the bending at every section between slices, as a push times an arm
// (newton second meters). Where the piece lies against a face (a contact
// with points apart along it) it is backed and cannot bend: those
// sections are left out, which is why a board that lands flat does not
// break and one that lands across a log, or on one end, may.
//
// At the worst section: the energy the two halves would take up turning
// against each other about it if it were a free hinge, but no more than
// what the hits had to give (energy_given). Against the energy the piece
// stores bent to breaking: its bending strength times how far it bends
// by then, which is stretch * length / (3 * depth) for a bar bent by a
// push at one point.
static void bend_loose_piece(wreck_model_t *model, int piece_index, float energy_given)
{
	static b3ContactData contacts[BEND_CONTACTS_MAX];
	const wreck_piece_t *piece = &model->pieces[piece_index];
	const wreck_group_t *group;
	b3Vec3 slice_pushes[BEND_SLICES];
	float span_starts[BEND_CONTACTS_MAX * 4];
	float span_ends[BEND_CONTACTS_MAX * 4];
	int span_count = 0;
	b3Transform frame;
	b3Vec3 axis;
	b3Vec3 push_sum = b3Vec3_zero;
	b3Vec3 turn_sum = b3Vec3_zero;
	b3Vec3 move;
	b3Vec3 turn;
	float half_sizes[3];
	float half_length;
	float length;
	float slice_length;
	float slice_mass;
	float turning_mass = 0.0f;
	float worst_squared = 0.0f;
	float worst_position = 0.0f;
	float before;
	float after;
	float mass_before;
	float mass_after;
	float lopsided;
	float hinge_ease;
	float energy;
	float energy_to_break;
	int contact_count;
	int contact_index;
	int manifold_index;
	int point_index;
	int section;
	int slice;
	int k;

	if (piece->group == WRECK_GONE || piece->long_axis < 0)
	{
		return;
	}
	group = &model->groups[piece->group];
	if (group->anchored || group->piece_count != 1)
	{
		return;
	}
	frame = b3Body_GetTransform(group->body);
	axis = b3RotateVector(frame.q, piece_axis(piece));
	piece_half_sizes(piece, half_sizes);
	half_length = half_sizes[piece->long_axis];
	length = 2.0f * half_length;
	slice_length = length / (float)BEND_SLICES;
	slice_mass = piece->mass / (float)BEND_SLICES;
	for (slice = 0; slice < BEND_SLICES; slice++)
	{
		slice_pushes[slice] = b3Vec3_zero;
	}

	// The pushes, each into its slice; and where the piece is backed
	contact_count = b3Body_GetContactData(group->body, contacts, BEND_CONTACTS_MAX);
	for (contact_index = 0; contact_index < contact_count; contact_index++)
	{
		const b3ContactData *contact = &contacts[contact_index];
		bool is_first = B3_ID_EQUALS(b3Shape_GetBody(contact->shapeIdA), group->body);

		for (manifold_index = 0; manifold_index < contact->manifoldCount; manifold_index++)
		{
			const b3Manifold *manifold = &contact->manifolds[manifold_index];
			float first_position = 0.0f;
			float last_position = 0.0f;
			int pushing_count = 0;

			for (point_index = 0; point_index < manifold->pointCount; point_index++)
			{
				const b3ManifoldPoint *point = &manifold->points[point_index];
				// (The normal goes from the first shape to the second)
				b3Vec3 push = b3MulSV(is_first ? -point->totalNormalImpulse : point->totalNormalImpulse, manifold->normal);
				float position = b3Dot(is_first ? point->anchorA : point->anchorB, axis);

				if (point->totalNormalImpulse <= 0.0f)
				{
					continue;
				}
				position = position < -half_length ? -half_length : (position > half_length ? half_length : position);
				slice = (int)((position + half_length) / slice_length);
				slice = slice >= BEND_SLICES ? BEND_SLICES - 1 : slice;
				push = b3Sub(push, b3MulSV(b3Dot(push, axis), axis));
				slice_pushes[slice] = b3Add(slice_pushes[slice], push);
				if (pushing_count == 0 || position < first_position)
				{
					first_position = position;
				}
				if (pushing_count == 0 || position > last_position)
				{
					last_position = position;
				}
				pushing_count++;
			}
			if (pushing_count >= 2 && span_count < BEND_CONTACTS_MAX * 4)
			{
				span_starts[span_count] = first_position;
				span_ends[span_count] = last_position;
				span_count++;
			}
		}
	}

	// How the pushes move and turn the piece as a whole
	for (slice = 0; slice < BEND_SLICES; slice++)
	{
		float position = -half_length + ((float)slice + 0.5f) * slice_length;

		push_sum = b3Add(push_sum, slice_pushes[slice]);
		turn_sum = b3Add(turn_sum, b3MulSV(position, b3Cross(axis, slice_pushes[slice])));
		turning_mass += slice_mass * position * position;
	}
	move = b3MulSV(1.0f / piece->mass, push_sum);
	turn = b3MulSV(1.0f / turning_mass, turn_sum);
	// What each slice gets beyond its share of that
	for (slice = 0; slice < BEND_SLICES; slice++)
	{
		float position = -half_length + ((float)slice + 0.5f) * slice_length;
		b3Vec3 share = b3MulSV(slice_mass, b3Add(move, b3MulSV(position, b3Cross(turn, axis))));

		slice_pushes[slice] = b3Sub(slice_pushes[slice], share);
	}
	// The bending at each section between slices: of everything before
	// it, the push times its arm to the section
	for (section = 1; section < BEND_SLICES; section++)
	{
		float section_position = -half_length + (float)section * slice_length;
		b3Vec3 bending = b3Vec3_zero;
		bool is_backed = false;

		for (k = 0; k < span_count; k++)
		{
			if (section_position > span_starts[k] && section_position < span_ends[k])
			{
				is_backed = true;
			}
		}
		if (is_backed)
		{
			continue;
		}
		for (slice = 0; slice < section; slice++)
		{
			float position = -half_length + ((float)slice + 0.5f) * slice_length;

			bending = b3Add(bending, b3MulSV(position - section_position, b3Cross(axis, slice_pushes[slice])));
		}
		if (b3Dot(bending, bending) > worst_squared)
		{
			worst_squared = b3Dot(bending, bending);
			worst_position = section_position;
		}
	}
	if (worst_squared <= 0.0f)
	{
		return;
	}

	// Two bars joined end to end by a pin, free otherwise, turned against
	// each other by a turning push at the pin: how readily they turn
	// (one over what stands for their mass in it). Each alone would be
	// 1 / (mass * length^2 / 12); the pin, which keeps their ends
	// together, takes some of that back when the bars are unlike
	before = worst_position + half_length;
	after = length - before;
	mass_before = piece->mass * before / length;
	mass_after = piece->mass * after / length;
	lopsided = 1.0f / (mass_after * after) - 1.0f / (mass_before * before);
	hinge_ease = 12.0f / (mass_before * before * before) + 12.0f / (mass_after * after * after) - 9.0f * lopsided * lopsided / (1.0f / mass_before + 1.0f / mass_after);
	energy = 0.5f * worst_squared * hinge_ease;
	if (energy > energy_given)
	{
		energy = energy_given;
	}

	// (Bending strength, bend * depth^2 * width, times how far it bends
	// by then, stretch * length / (3 * depth): the depth and the width
	// come together into the piece's volume, whichever way it is bent)
	energy_to_break = model->settings.bend[piece->material] * model->settings.stretch[piece->material] * piece->mass / model->settings.densities[piece->material] / 3.0f;
	if (energy > energy_to_break)
	{
		request_cut(model, piece_index, worst_position);
	}
}

void wreck_model_take_hit(wreck_model_t *model, const b3ContactHitEvent *event, int frame_index)
{
	int piece_a;
	int piece_b;
	float impulse = 0.0f;
	bool broke = false;
	b3ContactData contact;
	wreck_hit_t *hit;
	int side;
	int manifold_index;
	int point_index;

	if (!b3Shape_IsValid(event->shapeIdA) || !b3Shape_IsValid(event->shapeIdB) || !b3Contact_IsValid(event->contactId))
	{
		return;
	}
	// (A shape that is no piece of this model: the ground, a ball, a
	// piece of another building)
	piece_a = wreck_model_shape_piece(model, event->shapeIdA);
	piece_b = wreck_model_shape_piece(model, event->shapeIdB);
	if (piece_a < 0 && piece_b < 0)
	{
		return;
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
	// is long, and on its links (for a piece of a loose group of
	// several, not here: the shock goes through the whole group, below)
	for (side = 0; side < 2; side++)
	{
		int piece_index = side == 0 ? piece_a : piece_b;
		const wreck_group_t *group;

		if (piece_index < 0 || model->pieces[piece_index].group == WRECK_GONE)
		{
			continue;
		}
		if (hurt_piece(model, piece_index, impulse, frame_index))
		{
			broke = true;
		}
		group = &model->groups[model->pieces[piece_index].group];
		if (!group->anchored && group->piece_count == 1 && model->pieces[piece_index].long_axis >= 0)
		{
			// (A long piece loose by itself: looked at after the step,
			// with everything that touches it, in bend_loose_piece. What
			// this hit had to give: half its push by the speed it came
			// at, which is what a hit that does not bounce loses)
			model->bend_energies[piece_index] += 0.5f * impulse * event->approachSpeed;
			model->any_bend = true;
		}
		else if (snap_piece(model, piece_index, event->point, event->normal, impulse))
		{
			broke = true;
		}
		if (!group->anchored && !group->rubble && !group->rebuilt && group->piece_count > 1)
		{
			model->shock_groups[model->pieces[piece_index].group] = true;
			model->shock_pieces[piece_index] = true;
			model->any_shock = true;
		}
		else if (share_hit(model, piece_index, impulse, frame_index))
		{
			broke = true;
		}
	}
	if (broke && model->restored_count + 2 <= WRECK_RESTORED_BODIES_MAX)
	{
		model->restored_bodies[model->restored_count++] = b3Shape_GetBody(event->shapeIdA);
		model->restored_bodies[model->restored_count++] = b3Shape_GetBody(event->shapeIdB);
	}

	hit = &model->recent_hits[model->hit_count % WRECK_RECENT_HITS_MAX];
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

void wreck_model_after_step(wreck_model_t *model, int frame_index, float step_seconds)
{
	int i;

	if (model->any_bend)
	{
		// (Up to the count as it is now: a piece made by a cut here was
		// not hit)
		int piece_count = model->piece_count;

		for (i = 0; i < piece_count; i++)
		{
			if (model->bend_energies[i] > 0.0f)
			{
				bend_loose_piece(model, i, model->bend_energies[i]);
				model->bend_energies[i] = 0.0f;
			}
		}
		model->any_bend = false;
	}
	if (model->any_shock)
	{
		for (i = 0; i < WRECK_PIECES_MAX; i++)
		{
			if (model->shock_groups[i])
			{
				shock_through_group(model, i, frame_index, step_seconds);
				model->shock_groups[i] = false;
			}
		}
		memset(model->shock_pieces, 0, sizeof(model->shock_pieces));
		model->any_shock = false;
	}

	// (Only now: the events point at shapes the rebuilding destroys, and a
	// cut changes whose links are whose)
	make_pending_cuts(model);
	rebuild_dirty_groups(model);
	for (i = 0; i < model->restored_count; i++)
	{
		keep_going(model, model->restored_bodies[i]);
	}
	model->restored_count = 0;

	// What still stands: who bears what, if the graph changed; then what
	// is overloaded gives way a little more. A link that breaks here
	// makes the pass due again, at the next step: a collapse goes on
	// frame by frame.
	watch_water_on_standing(model);
	if (model->support_dirty)
	{
		work_out_support(model, frame_index);
	}
	give_way(model, frame_index, step_seconds);
	make_pending_cuts(model);
	rebuild_dirty_groups(model);
	remove_crushed(model);
	push_by_fluids(model, step_seconds);
}

bool wreck_model_is_busy(const wreck_model_t *model)
{
	int i;

	if (model->restored_count > 0 || model->any_shock || model->any_bend || model->support_dirty || model->any_group_dirty
		|| model->overloaded_count > 0 || model->pending_cut_count > 0)
	{
		return true;
	}
	// (With water about, what is loose and asleep is looked at too: the
	// water may have come to it)
	for (i = 0; i < WRECK_PIECES_MAX; i++)
	{
		const wreck_group_t *group = &model->groups[i];

		if (group->used && !group->anchored && (model->water_function != NULL || b3Body_IsAwake(group->body)))
		{
			return true;
		}
	}
	return false;
}

b3Transform wreck_model_piece_transform(const wreck_model_t *model, int piece_index)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	b3Transform frame = b3Body_GetTransform(model->groups[piece->group].body);
	b3Transform result;

	result.p = b3Add(frame.p, b3RotateVector(frame.q, piece->local.p));
	result.q = b3MulQuat(frame.q, piece->local.q);
	return result;
}

// How much of a piece faces a direction, square meters: what a blast
// from there pushes on
static float exposed_area(const wreck_piece_t *piece, b3Quat rotation, b3Vec3 direction)
{
	if (piece->shape == WRECK_PIECE_BOX)
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

// How much of what a blast's wave gives at 1 m (its strength, newton
// seconds a square meter) it gives at a point, and which way it pushes
// there. It falls off with the distance to the power blast_falloff_power
// (1: as measured blasts do, see DESIGN.md), with no end to it; nearer
// than the bomb is big it grows no more. How big the bomb is comes from
// its strength: the kilograms of explosive that give that much at 1 m
// (blast_impulse_per_kilogram), at blast_charge_density.
static float blast_reach(const wreck_model_t *model, float strength, b3Vec3 blast_position, b3Vec3 point, b3Vec3 *direction)
{
	b3Vec3 offset = b3Sub(point, blast_position);
	float distance = b3Length(offset);
	float charge_kilograms = powf(strength / model->settings.blast_impulse_per_kilogram, 1.5f);
	float charge_radius = cbrtf(3.0f * charge_kilograms / (4.0f * PI * model->settings.blast_charge_density));

	*direction = distance > 1e-4f ? b3MulSV(1.0f / distance, offset) : make_vector(0.0f, 1.0f, 0.0f);
	if (distance < charge_radius)
	{
		distance = charge_radius;
	}
	return powf(distance, -model->settings.blast_falloff_power);
}

// A ray through the world that takes no notice of what can move and is
// no model's: a barrel, an animal, a ball (they neither hide anything
// from a blast nor shut its gas in). True when it met something else:
// a piece of some model, or something that cannot move. shape: what it
// met; point (may be NULL): where.
static bool cast_ray_past_movables(const wreck_model_t *model, b3Vec3 origin, b3Vec3 translation, b3ShapeId *shape, b3Vec3 *point)
{
	float length = b3Length(translation);
	b3Vec3 direction;
	b3Vec3 start = origin;
	float left = length;
	int pass;

	if (length < 1e-6f)
	{
		return false;
	}
	direction = b3MulSV(1.0f / length, translation);
	for (pass = 0; pass < RAY_PASSES && left > 0.01f; pass++)
	{
		b3RayResult result = b3World_CastRayClosest(model->world, start, b3MulSV(left, direction), b3DefaultQueryFilter());

		if (!result.hit)
		{
			return false;
		}
		if (wreck_shape_model(result.shapeId) != NULL || b3Body_GetType(b3Shape_GetBody(result.shapeId)) == b3_staticBody)
		{
			*shape = result.shapeId;
			if (point != NULL)
			{
				*point = result.point;
			}
			return true;
		}
		left -= b3Length(b3Sub(result.point, start)) + RAY_STEP_METERS;
		start = b3Add(result.point, b3MulSV(RAY_STEP_METERS, direction));
	}
	return false;
}

// What a blast gives a piece, newton seconds and which way. A long piece
// is taken in parts along its length, each pushed from where the blast
// is by how far it is, how much of it faces the blast, and whether
// another piece or the ground is between it and the blast (then it gets
// what blast_shielding leaves): a blast against a wall pushes hard the
// part of a board that is in front of it, whatever way the board's
// middle is. is_gas: the gas that is left after the wave instead, which
// fills the place: it does not fade with distance and is not shielded.
// turning: how that push turns the piece about its middle (a push on
// one end of a board is borne by the nails at that end).
static b3Vec3 blast_on_piece(const wreck_model_t *model, b3Vec3 position, float strength, int piece_index, bool is_gas, b3Vec3 *turning)
{
	const wreck_piece_t *piece = &model->pieces[piece_index];
	b3Transform transform = wreck_model_piece_transform(model, piece_index);
	b3Vec3 along = b3Vec3_zero;
	b3Vec3 push = b3Vec3_zero;
	int part_count = 1;
	int part;

	*turning = b3Vec3_zero;

	if (piece->long_axis >= 0)
	{
		b3Vec3 own_axis = { piece->long_axis == 0 ? 1.0f : 0.0f, piece->long_axis == 1 ? 1.0f : 0.0f, piece->long_axis == 2 ? 1.0f : 0.0f };
		float half_sizes[3];

		piece_half_sizes(piece, half_sizes);
		along = b3MulSV(half_sizes[piece->long_axis], b3RotateVector(transform.q, own_axis));
		part_count = BLAST_PARTS;
	}
	for (part = 0; part < part_count; part++)
	{
		b3Vec3 place = b3Add(transform.p, b3MulSV((2.0f * (float)part + 1.0f) / (float)part_count - 1.0f, along));
		b3Vec3 direction;
		float reach = blast_reach(model, strength, position, place, &direction);
		float impulse = strength * (is_gas ? 1.0f : reach) * exposed_area(piece, transform.q, direction) / (float)part_count;

		if (impulse > 0.0f && !is_gas && model->settings.blast_shielding > 0.0f)
		{
			b3ShapeId shape;

			// (In the way: another piece of this model or of another, or
			// something of the world's that cannot move)
			if (cast_ray_past_movables(model, position, b3Sub(place, position), &shape, NULL) && wreck_model_shape_piece(model, shape) != piece_index)
			{
				impulse *= 1.0f - model->settings.blast_shielding;
			}
		}
		push = b3Add(push, b3MulSV(impulse, direction));
		*turning = b3Add(*turning, b3Cross(b3Sub(place, transform.p), b3MulSV(impulse, direction)));
	}
	return push;
}

// A push on every piece (pushes[], each piece's whole push, and
// turnings[], how it turns the piece about its middle, in the world's
// frame), come on in slices (see wreck_model_blast). Leaves in
// blast_shares[] the part of its push that was still to come when a
// piece was no longer held to the ground (1 for what was not to begin
// with; of no use for what still stands).
static void push_through_links(wreck_model_t *model, const b3Vec3 *pushes, const b3Vec3 *turnings, int piece_count_before, int frame_index)
{
	int slice_count = model->settings.blast_slices > 1 ? model->settings.blast_slices : 1;
	int slice;
	int i;

	find_standing(model);
	for (i = 0; i < piece_count_before; i++)
	{
		blast_was_standing[i] = support_unknowns[i] >= 0;
		blast_shares[i] = 1.0f;
	}
	for (slice = 1; slice <= slice_count + 1; slice++)
	{
		// (One more round after the last slice, only to see what the last
		// slice's breaks let go of)
		double come_on = (double)(slice < slice_count ? slice : slice_count) / (double)slice_count;
		int standing_count;
		double load_squared = 0.0;

		// What is no longer held to the ground since the slice before: it
		// will be thrown by the slices still to come
		find_standing(model);
		for (i = 0; i < piece_count_before; i++)
		{
			if (blast_was_standing[i] && support_unknowns[i] < 0)
			{
				blast_was_standing[i] = false;
				blast_shares[i] = 1.0f - (float)(slice - 1) / (float)slice_count;
			}
		}
		if (slice > slice_count)
		{
			break;
		}

		// Everything is solved, loose or not: a loose house is strained by
		// a blast inside it as a standing one is. (standing_count: all
		// the pieces there are, from here on.)
		standing_count = 0;
		for (i = 0; i < model->piece_count; i++)
		{
			support_unknowns[i] = -1;
			if (model->pieces[i].group != WRECK_GONE && !model->pieces[i].crushed)
			{
				support_unknowns[i] = standing_count;
				walk_queue[standing_count++] = i;
			}
		}

		// (A blast is over before much can move: each piece's own mass
		// takes up part of its push, and of what its links pass to it, as
		// a spring to where it stands would; so the push fades from link
		// to link instead of all going down to the ground)
		for (i = 0; i < standing_count; i++)
		{
			int piece_index = walk_queue[i];
			const wreck_piece_t *piece = &model->pieces[piece_index];
			float half_sizes[3];
			double held = (double)model->settings.blast_inertia * (double)piece->mass;
			double reach_squared;
			int k;

			piece_half_sizes(piece, half_sizes);
			reach_squared = ((double)half_sizes[0] * (double)half_sizes[0] + (double)half_sizes[1] * (double)half_sizes[1] + (double)half_sizes[2] * (double)half_sizes[2]) / 3.0;
			for (k = 0; k < 6; k++)
			{
				solver_load[i * 6 + k] = 0.0;
				solver_x[i * 6 + k] = 0.0;
				solver_inertia[i * 6 + k] = k < 3 ? held : held * reach_squared;
			}
			// (The push turned into the frame of the piece's body, which
			// for what stands is the world's)
			if (piece_index < piece_count_before)
			{
				b3Vec3 push = b3InvRotateVector(b3Body_GetRotation(model->groups[piece->group].body), pushes[piece_index]);
				b3Vec3 turning = b3InvRotateVector(b3Body_GetRotation(model->groups[piece->group].body), turnings[piece_index]);

				solver_load[i * 6 + 0] = come_on * (double)push.x;
				solver_load[i * 6 + 1] = come_on * (double)push.y;
				solver_load[i * 6 + 2] = come_on * (double)push.z;
				solver_load[i * 6 + 3] = come_on * (double)turning.x;
				solver_load[i * 6 + 4] = come_on * (double)turning.y;
				solver_load[i * 6 + 5] = come_on * (double)turning.z;
				load_squared += solver_load[i * 6 + 0] * solver_load[i * 6 + 0] + solver_load[i * 6 + 1] * solver_load[i * 6 + 1] + solver_load[i * 6 + 2] * solver_load[i * 6 + 2];
			}
		}
		if (load_squared <= 0.0)
		{
			break;
		}
		solve_springs(model, standing_count, load_squared);

		for (i = 0; i < model->link_count; i++)
		{
			const wreck_link_t *link = &model->links[i];
			double apart[3];
			double turned[3];
			float harm;

			if (link->broken || support_unknowns[link->piece_a] < 0)
			{
				continue;
			}
			// (Its turning counts as the push that would turn it so at
			// joint_arm)
			link_strain(model, link, solver_x, apart, turned);
			harm = link_harm_from_strain(model, link, apart, turned);
			if (slice == slice_count)
			{
				hurt_link(model, i, harm, frame_index);
			}
			else if (harm >= link->hit_points_left)
			{
				break_link(model, i, frame_index);
			}
		}
		for (i = 0; i < standing_count; i++)
		{
			int piece_index = walk_queue[i];
			const wreck_piece_t *piece = &model->pieces[piece_index];
			double own_push[3];
			double worst_position = 0.0;
			int k;

			if (piece->long_axis < 0 || piece->crushed || piece_index >= piece_count_before)
			{
				continue;
			}
			// (Its push less what its own mass took up, both spread along it)
			for (k = 0; k < 3; k++)
			{
				own_push[k] = solver_load[i * 6 + k] - solver_inertia[i * 6 + k] * solver_x[i * 6 + k];
			}
			if (piece_bending(model, piece_index, solver_x, own_push, (double)model->settings.snap[piece->material], &worst_position) > 1.0)
			{
				request_cut(model, piece_index, (float)worst_position);
			}
		}
	}
}

// After the blast's wave, what came out of the bomb is still there: gas
// that presses on everything round it until it finds a way out. Rays
// from the blast, evenly all round, look for the ways out: one that
// ends on a standing piece is shut in, one that meets nothing (or only
// pieces the wave broke loose, which it goes through) got out; the
// ground is neither. From the part of them that is shut in comes how
// hard the gas presses on all it reaches (blast_gas times the blast's
// strength when all are, falling off fast as ways out open:
// blast_gas_venting); a piece next to a way out, where the gas rushes
// past, gets more (blast_gas_rush). That push is then put on what
// stands as the wave's was. In the open there is nothing to shut the
// gas in and this does nothing.
static void gas_after_blast(wreck_model_t *model, b3Vec3 position, float strength, int piece_count_before, int frame_index)
{
	const float golden_angle = PI * (3.0f - sqrtf(5.0f));
	float reach = GAS_RAY_METERS;
	int shut_in = 0;
	int out = 0;
	float shut_part;
	float pressing;
	bool any = false;
	int ray;
	int other;
	int i;

	if (model->settings.blast_gas <= 0.0f)
	{
		return;
	}
	// How many pieces each piece is still linked with, itself counted:
	// what the wave left in bits too small to be rubble shuts nothing in
	for (i = 0; i < model->piece_count; i++)
	{
		gas_island_sizes[i] = 0;
	}
	for (i = 0; i < model->piece_count; i++)
	{
		int queue_start = 0;
		int queue_end = 0;
		int k;

		if (gas_island_sizes[i] != 0 || model->pieces[i].group == WRECK_GONE)
		{
			continue;
		}
		gas_island_sizes[i] = -1;
		walk_queue[queue_end++] = i;
		while (queue_start < queue_end)
		{
			int piece_index = walk_queue[queue_start++];

			for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
			{
				const wreck_link_t *link = &model->links[model->piece_links[k]];
				int next;

				if (link->broken || link->piece_b == WRECK_GROUND)
				{
					continue;
				}
				next = link->piece_a == piece_index ? link->piece_b : link->piece_a;
				if (gas_island_sizes[next] == 0)
				{
					gas_island_sizes[next] = -1;
					walk_queue[queue_end++] = next;
				}
			}
		}
		for (k = 0; k < queue_end; k++)
		{
			gas_island_sizes[walk_queue[k]] = queue_end;
		}
	}
	for (ray = 0; ray < GAS_RAYS; ray++)
	{
		// (Evenly over a ball: up it in equal steps, round it by the
		// golden angle)
		float height = 1.0f - 2.0f * ((float)ray + 0.5f) / (float)GAS_RAYS;
		float ring = sqrtf(1.0f - height * height);
		b3Vec3 direction = make_vector(ring * cosf(golden_angle * (float)ray), height, ring * sinf(golden_angle * (float)ray));
		b3Vec3 start = position;
		float left = reach;
		int pass;

		gas_ray_directions[ray] = direction;
		gas_ray_pieces[ray] = GAS_RAY_OUT;
		for (pass = 0; pass < GAS_RAY_PASSES && left > 0.01f; pass++)
		{
			b3ShapeId shape;
			b3Vec3 point;
			int piece_index;

			if (!cast_ray_past_movables(model, start, b3MulSV(left, direction), &shape, &point))
			{
				break;
			}
			piece_index = wreck_model_shape_piece(model, shape);
			if (piece_index >= 0 && piece_index < piece_count_before && gas_island_sizes[piece_index] >= model->settings.rubble_pieces)
			{
				gas_ray_pieces[ray] = piece_index;
				break;
			}
			if (piece_index < 0)
			{
				// (A piece of another building is a wall; what cannot
				// move and is no model's, the ground or a tree, is
				// neither a wall nor a way out)
				gas_ray_pieces[ray] = wreck_shape_model(shape) != NULL ? GAS_RAY_WALL : GAS_RAY_GROUND;
				break;
			}
			// (A bit of this building the wave broke off: on through it)
			left -= b3Length(b3Sub(point, start)) + RAY_STEP_METERS;
			start = b3Add(point, b3MulSV(RAY_STEP_METERS, direction));
		}
		if (gas_ray_pieces[ray] >= 0 || gas_ray_pieces[ray] == GAS_RAY_WALL)
		{
			shut_in++;
		}
		else if (gas_ray_pieces[ray] == GAS_RAY_OUT)
		{
			out++;
		}
	}
	if (shut_in == 0)
	{
		return;
	}
	shut_part = (float)shut_in / (float)(shut_in + out);
	pressing = model->settings.blast_gas * powf(shut_part, model->settings.blast_gas_venting);

	// Each piece a ray ends on: more if rays near that one got out
	for (i = 0; i < piece_count_before; i++)
	{
		gas_factors[i] = 0.0f;
		gas_impulses[i] = b3Vec3_zero;
		gas_turnings[i] = b3Vec3_zero;
	}
	for (ray = 0; ray < GAS_RAYS; ray++)
	{
		int near_count = 0;
		int near_out = 0;
		float factor;

		if (gas_ray_pieces[ray] < 0)
		{
			continue;
		}
		for (other = 0; other < GAS_RAYS; other++)
		{
			if (other != ray && gas_ray_pieces[other] != GAS_RAY_GROUND && b3Dot(gas_ray_directions[ray], gas_ray_directions[other]) > GAS_NEIGHBOR_COSINE)
			{
				near_count++;
				if (gas_ray_pieces[other] == GAS_RAY_OUT)
				{
					near_out++;
				}
			}
		}
		factor = pressing * (1.0f + (near_count > 0 ? model->settings.blast_gas_rush * (float)near_out / (float)near_count : 0.0f));
		if (factor > gas_factors[gas_ray_pieces[ray]])
		{
			gas_factors[gas_ray_pieces[ray]] = factor;
		}
	}
	for (i = 0; i < piece_count_before; i++)
	{
		if (gas_factors[i] > 0.0f)
		{
			gas_impulses[i] = blast_on_piece(model, position, strength * gas_factors[i], i, true, &gas_turnings[i]);
			any = true;
		}
	}
	if (!any)
	{
		return;
	}
	push_through_links(model, gas_impulses, gas_turnings, piece_count_before, frame_index);
	for (i = 0; i < piece_count_before; i++)
	{
		// (What the gas pressed on and is not held to the ground by the
		// end is thrown by what of it came on after it let go)
		if (gas_factors[i] > 0.0f && !blast_was_standing[i])
		{
			model->pieces[i].gas_push_left = gas_factors[i] * blast_shares[i];
		}
	}
}

// A blast. Every piece gets its push (blast_on_piece), which costs the
// piece itself hit points. What is loose already is hurt piece by piece,
// as by a hit, and gets all of its push.
//
// For what stands, the push comes on in slices (blast_slices). In each,
// what has come on so far is put on the pieces still standing, all at
// once, as their weights are in the support pass, and the springs
// solved: what goes through a link is what strains it (two neighbors
// pushed alike do not strain the link between them; the links that hold
// the pushed part to the rest, and to the ground, bear it all; but each
// piece's own mass takes up some on the way: blast_inertia). A link
// that by then has borne more than it can breaks; the next slice is
// solved without it, so others may follow. A long piece bent by more
// than it bears from a hit is cut. A piece that comes loose in a slice
// is pushed only by the slices still to come: what came before went
// through its links to the ground. Links that held to the end keep the
// harm of the whole push.
void wreck_model_blast(wreck_model_t *model, b3Vec3 position, float strength, int frame_index)
{
	int piece_count_before = model->piece_count;
	int i;

	for (i = 0; i < piece_count_before; i++)
	{
		float impulse;

		model->pieces[i].blast_push_left = 1.0f;
		model->pieces[i].gas_push_left = 0.0f;
		blast_turnings[i] = b3Vec3_zero;
		blast_impulses[i] = model->pieces[i].group == WRECK_GONE ? b3Vec3_zero : blast_on_piece(model, position, strength, i, false, &blast_turnings[i]);
		impulse = b3Length(blast_impulses[i]);
		if (impulse > 0.0f)
		{
			hurt_piece(model, i, impulse, frame_index);
		}
	}

	push_through_links(model, blast_impulses, blast_turnings, piece_count_before, frame_index);
	for (i = 0; i < piece_count_before; i++)
	{
		model->pieces[i].blast_push_left = blast_shares[i];
	}
	gas_after_blast(model, position, strength, piece_count_before, frame_index);
	make_pending_cuts(model);
	rebuild_dirty_groups(model);

	// The push on what is loose now (worked out again: pieces were cut),
	// less what came on while the piece still stood
	for (i = 0; i < model->piece_count; i++)
	{
		const wreck_piece_t *piece = &model->pieces[i];
		const wreck_group_t *group;
		b3Vec3 push;
		b3Vec3 turning;
		b3Vec3 gas_turning;

		if (piece->group == WRECK_GONE)
		{
			continue;
		}
		group = &model->groups[piece->group];
		if (group->anchored)
		{
			continue;
		}
		push = b3MulSV(piece->blast_push_left, blast_on_piece(model, position, strength, i, false, &turning));
		turning = b3MulSV(piece->blast_push_left, turning);
		if (piece->gas_push_left > 0.0f)
		{
			push = b3Add(push, blast_on_piece(model, position, strength * piece->gas_push_left, i, true, &gas_turning));
			turning = b3Add(turning, gas_turning);
		}
		if (b3Length(push) > 0.0f)
		{
			b3Body_ApplyLinearImpulse(group->body, push, wreck_model_piece_transform(model, i).p, true);
			b3Body_ApplyAngularImpulse(group->body, turning, true);
		}
	}
	for (i = 0; i < model->projectile_count; i++)
	{
		const wreck_projectile_t *projectile = &model->projectiles[i];
		b3Vec3 center = b3Body_GetPosition(projectile->body);
		b3Vec3 direction;
		float reach = blast_reach(model, strength, position, center, &direction);

		if (reach > 0.0f)
		{
			b3Body_ApplyLinearImpulseToCenter(projectile->body, b3MulSV(strength * reach * PI * projectile->radius * projectile->radius, direction), true);
		}
	}
}

float wreck_blast_strength(const wreck_settings_t *settings, float kilograms)
{
	return settings->blast_impulse_per_kilogram * powf(kilograms, 2.0f / 3.0f);
}

void wreck_model_blast_kilograms(wreck_model_t *model, b3Vec3 position, float kilograms, int frame_index)
{
	wreck_model_blast(model, position, wreck_blast_strength(&model->settings, kilograms), frame_index);
}

// ---------------------------------------------------------------------
// Looking at it

wreck_model_t *wreck_shape_model(b3ShapeId shape)
{
	// (Only a piece's shape carries user data: the engine puts none on
	// its own shapes, and that is how the two are told apart)
	if (!b3Shape_IsValid(shape) || b3Shape_GetUserData(shape) == NULL)
	{
		return NULL;
	}
	return (wreck_model_t *)b3Body_GetUserData(b3Shape_GetBody(shape));
}

wreck_model_t *wreck_body_model(b3BodyId body)
{
	b3ShapeId shape;

	if (!b3Body_IsValid(body) || b3Body_GetShapes(body, &shape, 1) < 1)
	{
		return NULL;
	}
	return wreck_shape_model(shape);
}

int wreck_model_shape_piece(const wreck_model_t *model, b3ShapeId shape)
{
	if (wreck_shape_model(shape) != model)
	{
		return -1;
	}
	return (int)(intptr_t)b3Shape_GetUserData(shape) - 1;
}

void wreck_model_describe(const wreck_model_t *model, char *text, int text_size)
{
	int standing_count = 0;
	int loose_count = 0;
	int rubble_count = 0;
	int hurt_count = 0;
	int i;

	for (i = 0; i < WRECK_PIECES_MAX; i++)
	{
		const wreck_group_t *group = &model->groups[i];

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
		const wreck_link_t *link = &model->links[i];

		if (!link->broken && link->hit_points_left < 1.0f)
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
uint32_t wreck_model_checksum(const wreck_model_t *model)
{
	uint32_t hash = 2166136261u;
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		b3Transform transform;
		float values[7];

		if (model->pieces[i].group == WRECK_GONE)
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
